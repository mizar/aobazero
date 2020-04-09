// 2019 Team AobaZero
// This source code is in the public domain.
#ifdef _MSC_VER
#  define _CRT_SECURE_NO_WARNINGS
#endif
#include "child.hpp"
#include "err.hpp"
#include "iobase.hpp"
#include "nnet-ipc.hpp"
#include "osi.hpp"
#include "param.hpp"
#include "play.hpp"
#include "shogibase.hpp"
#include <chrono>
#include <queue>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <utility>
#include <cassert>
#include <cctype>
#include <cinttypes>
#include <climits>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
using std::cout;
using std::deque;
using std::endl;
using std::flush;
using std::ios;
using std::lock_guard;
using std::move;
using std::mutex;
using std::ofstream;
using std::queue;
using std::shared_ptr;
using std::string;
using std::to_string;
using std::unique_ptr;
using std::vector;
using std::chrono::steady_clock;
using std::chrono::duration_cast;
using std::chrono::milliseconds;
using ErrAux::die;
using namespace IOAux;
using uint = unsigned int;

constexpr double time_average_rate = 0.999;
constexpr char fmt_log[]           = "engine%03u-%03u.log";
static mutex m_seq;
static deque<SeqPRNService> seq_s;

class Device {
  enum class Type : unsigned char { Aobaz, NNService, Bad };
  class DataNNService {
    NNetService _nnet;
  public:
    DataNNService(uint nnet_id, uint size_parallel, uint size_batch, uint id,
		  uint use_half) noexcept
    : _nnet(nnet_id, size_parallel, size_batch, id, use_half) {}
    void nnreset(const FName &fname) noexcept { _nnet.nnreset(fname); }
    void flush_on() noexcept { _nnet.flush_on(); }
    void flush_off() noexcept { _nnet.flush_off(); }
  };
  unique_ptr<DataNNService> _data_nnservice;
  int _nnet_id, _device_id;
  uint _size_parallel;
  uint _size_batch;
  bool _flag_half;
  Type _type;

public:
  static constexpr Type aobaz     = Type::Aobaz;
  static constexpr Type nnservice = Type::NNService;
  static constexpr Type bad       = Type::Bad;
  explicit Device(const string &s, int nnet_id) noexcept
    : _nnet_id(-1), _size_parallel(1U), _flag_half(false), _type(bad) {
    if (s.empty()) die(ERR_INT("invalid device %s", s.c_str()));
    char *endptr;
    if (s[0] == 'S' || s[0] == 's') {
      {
	lock_guard<mutex> lock(m_seq);
	if (seq_s.empty()) seq_s.emplace_back(); }
      const char *token = s.c_str() + 1;
      long int device_id = strtol(token, &endptr, 10);
      if (endptr == token || *endptr != ':' || device_id < -1
	  || device_id == LONG_MAX) die(ERR_INT("invalid device %s", token));
      token = endptr + 1;
      
      long int size_batch = strtol(token, &endptr, 10);
      if (endptr == token || *endptr != ':' || size_batch < 1
	  || size_batch == LONG_MAX) die(ERR_INT("invalid device %s",
						 s.c_str()));
      token = endptr + 1;
      
      long int size_parallel = strtol(token, &endptr, 10);
      if ((*endptr != '\0' && *endptr != 'H' && *endptr != 'h')
	  || endptr == token || size_parallel < 1 || size_parallel == LONG_MAX)
	die(ERR_INT("invalid device %s", s.c_str()));
      
      if (*endptr == '\0') _flag_half = false;
      else if ((*endptr == 'H' || *endptr == 'h') && endptr[1] == '\0')
	_flag_half = true;

      _device_id     = device_id;
      _nnet_id       = nnet_id;
      _type          = nnservice;
      _size_parallel = size_parallel;
      _size_batch    = size_batch;
      _data_nnservice.reset(new DataNNService(_nnet_id, _size_parallel,
					      _size_batch, _device_id,
					      _flag_half)); }
    else {
      const char *token = s.c_str();
      _nnet_id = strtol(token, &endptr, 10);
      if (endptr == token || *endptr != '\0' || _nnet_id < -1
	  || 65535 < _nnet_id) die(ERR_INT("invalid device %s", s.c_str()));
      _device_id = _nnet_id;
      _type = aobaz; } }
  void nnreset(const FName &wfname) noexcept {
    if (_type == nnservice) _data_nnservice->nnreset(wfname); }
  int get_device_id() const noexcept { return _device_id; }
  int get_nnet_id() const noexcept { return _nnet_id; }
  char get_id_option_character() const noexcept {
    return _type == nnservice ? 'e' : 'u'; }
  uint get_size_parallel() const noexcept { return _size_parallel; }
  void flush_on() const noexcept {
    if (_type == nnservice) _data_nnservice->flush_on(); }
  void flush_off() const noexcept {
    if (_type == nnservice) _data_nnservice->flush_off(); }
};
constexpr Device::Type Device::aobaz;
constexpr Device::Type Device::nnservice;
constexpr Device::Type Device::bad;

class USIEngine : public Child {
  using uint = unsigned int;
  Node<Param::maxlen_play_learn> _node;
  steady_clock::time_point _time_last;
  double _time_average_nume, _time_average_deno, _time_average;
  FName _logname;
  ofstream _ofs;
  string _startpos, _record, _record_header, _fingerprint, _settings;
  int _device_id, _nnet_id, _version;
  uint _eid, _nmove;
  bool _flag_playing, _flag_ready, _flag_usistart;

public:
  explicit USIEngine(const FName &cname, char ch, int device_id, int nnet_id,
		     uint eid, const FNameID &wfname, uint64_t crc64,
		     uint verbose_eng, const FName &logname) noexcept
    : _time_average_nume(0.0), _time_average_deno(0.0), 
    _time_average(0.0), _logname(logname),
    _fingerprint(to_string(nnet_id) + string("-") + to_string(eid)),
    _device_id(device_id), _nnet_id(nnet_id), _version(-1), _eid(eid),
    _flag_playing(false), _flag_ready(false), _flag_usistart(false) {
    assert(cname.ok() && 0 < cname.get_len_fname());
    assert(wfname.ok() && 0 < wfname.get_len_fname());
    assert(isalnum(ch) && -2 < nnet_id && nnet_id < 65536);
    char *argv[256];
    int argc = 0;

    unique_ptr<char []> a0(new char [cname.get_len_fname() + 1U]);
    memcpy(a0.get(), cname.get_fname(), cname.get_len_fname() + 1U);
    argv[argc++] = a0.get();

    char opt_q[] = "-q";
    if (!verbose_eng) argv[argc++] = opt_q;

    char opt_p[]       = "-p";
    char opt_p_value[] = "800";
    argv[argc++] = opt_p;
    argv[argc++] = opt_p_value;

    char opt_n[] = "-n";
    argv[argc++] = opt_n;

    char opt_m[]       = "-m";
    char opt_m_value[] = "30";
    argv[argc++] = opt_m;
    argv[argc++] = opt_m_value;

    char opt_w[] = "-w";
    unique_ptr<char []> opt_w_value(new char [wfname.get_len_fname() + 1U]);
    memcpy(opt_w_value.get(),
	   wfname.get_fname(), wfname.get_len_fname() + 1U);
    argv[argc++] = opt_w;
    argv[argc++] = opt_w_value.get();

    char opt_u[] = "-u";
    char opt_u_value[256];
    opt_u[1] = ch;
    sprintf(opt_u_value, "%i", nnet_id);
    argv[argc++] = opt_u;
    argv[argc++] = opt_u_value;

    assert(argc < 256);
    unique_ptr<char []> path(new char [cname.get_len_fname() + 1U]);
    memcpy(path.get(), cname.get_fname(), cname.get_len_fname() + 1U);
    argv[argc] = nullptr;
    Child::open(path.get(), argv);
  
    _logname.add_fmt_fname(fmt_log, nnet_id, eid);
    _ofs.open(_logname.get_fname(), ios::trunc);
    if (!_ofs) die(ERR_INT("cannot write to log"));

    char buf[256];
    snprintf(buf, sizeof(buf), "%16" PRIx64, crc64);
    _record_header  = string("'w ") + to_string(wfname.get_id());
    _record_header += string(" (crc64:") + string(buf);
    _record_header += string("), autousi ") + to_string(Ver::major);
    _record_header += string(".") + to_string(Ver::minor); }

  void out_log(const char *p) noexcept {
    assert(_ofs && p);
    _ofs << p << endl;
    if (!_ofs) die(ERR_INT("cannot write to log")); }

  void start_newgame() noexcept {
    _node.clear();
    _flag_playing = true;
    _record       = _record_header;
    _nmove        = 0;
    _startpos     = string("position startpos moves");
    engine_out("usinewgame");
    engine_out("%s", _startpos.c_str());
    _time_last = steady_clock::now();
    engine_out("go visit"); }

  void start_usi() noexcept { _flag_usistart = true; engine_out("usi"); }

  string update(char *line, queue<string> &moves_eid0) noexcept {
    assert(line);
    char *token, *saveptr;
    token = OSI::strtok(line, " ,", &saveptr);

    if (!_flag_ready && strcmp(line, "usiok") == 0) {
      _flag_ready = true;
      if (_version < 0) die(ERR_INT("No version infomation from engine %s",
				    get_fp()));
      _record_header += string(", usi-engine ") + to_string(_version);
      _record_header += string("\n'") + _settings;
      _record_header += string("\nPI\n+\n");
      engine_out("isready");
      return string(""); }

    if (!_flag_ready && strcmp(token, "id") == 0) {
      token = OSI::strtok(nullptr, " ", &saveptr);
      if (!token) die(ERR_INT("Bad message from engine (%s).", get_fp()));

      if (strcmp(token, "settings") == 0) {
	token = OSI::strtok(nullptr, "", &saveptr);
	if (!token) die(ERR_INT("Bad message from engine (%s).", get_fp()));
	_settings = string(token); }

      else if (strcmp(token, "name") == 0) {
	if (! OSI::strtok(nullptr, " ", &saveptr))
	  die(ERR_INT("Bad message from engine (%s).", get_fp()));
	  
	token = OSI::strtok(nullptr, " ", &saveptr);
	if (!token) die(ERR_INT("Bad message from engine (%s).", get_fp()));
	  
	char *endptr;
	long int ver = strtol(token, &endptr, 10);
	if (endptr == token || *endptr != '\0' || ver < 0 || 65535 < ver)
	  die(ERR_INT("Bad message from engine (%s).", get_fp()));
	_version = ver; }

      return string(""); }

    if (!_flag_ready) die(ERR_INT("Bad message from engine (%s).", get_fp()));

    if (strcmp(token, "bestmove") != 0) return string("");
    if (!_flag_playing)
      die(ERR_INT("bad usi message from engine %s", get_fp()));

    // read played move
    long int num_best = 0;
    long int num_tot  = 0;
    const char *str_move_usi = OSI::strtok(nullptr, " ,", &saveptr);
    if (!str_move_usi)
      die(ERR_INT("bad usi message from engine %s", get_fp()));

    Action actionPlay = _node.action_interpret(str_move_usi, SAux::usi);
    if (!actionPlay.ok())
      die(ERR_INT("cannot interpret candidate move %s (engine %s)\n%s",
		  str_move_usi, get_fp(),
		  static_cast<const char *>(_node.to_str())));

    if (actionPlay.is_move()) {
      steady_clock::time_point time_now = steady_clock::now();
      auto rep   = duration_cast<milliseconds>(time_now - _time_last).count();
      double dms = static_cast<double>(rep);
      _time_last = time_now;
      _time_average_nume = _time_average_nume * time_average_rate + dms;
      _time_average_deno = _time_average_deno * time_average_rate + 1.0;
      _time_average      = _time_average_nume / _time_average_deno;
      _startpos         += " ";
      _startpos         += str_move_usi;
      _record           += _node.get_turn().to_str();
      _nmove            += 1U;
      _record           += actionPlay.to_str(SAux::csa);
      if (_eid == 0) {
	char buf[256];
	sprintf(buf, " (%5.0fms)", _time_average);
	string smove = _node.get_turn().to_str();
	smove += actionPlay.to_str(SAux::csa);
	smove += buf;
	moves_eid0.push(std::move(smove)); }
    
      const char *str_count = OSI::strtok(nullptr, " ,", &saveptr);
      if (!str_count) die(ERR_INT("cannot read count (engine %s)", get_fp()));
    
      char *endptr;
      long int num = strtol(str_count, &endptr, 10);
      if (endptr == str_count || *endptr != '\0' || num < 1
	  || num == LONG_MAX)
	die(ERR_INT("cannot interpret a visit count %s (engine %s)",
		    str_count, get_fp()));
    
      num_best = num;
      _record += ",'";
      _record += to_string(num);

      // read candidate moves
      while (true) {
	str_move_usi = OSI::strtok(nullptr, " ,", &saveptr);
	if (!str_move_usi) { _record += "\n"; break; }

	Action action = _node.action_interpret(str_move_usi, SAux::usi);
	if (!action.is_move())
	  die(ERR_INT("bad candidate %s (engine %s)", str_move_usi, get_fp()));
	_record += ",";
	_record += action.to_str(SAux::csa);
    
	str_count = OSI::strtok(nullptr, " ,", &saveptr);
	if (!str_count)
	  die(ERR_INT("cannot read count (engine %s)", get_fp()));

	num = strtol(str_count, &endptr, 10);
	if (endptr == str_count || *endptr != '\0'
	    || num < 1 || num == LONG_MAX)
	  die(ERR_INT("cannot interpret a visit count %s (engine %s)",
		      str_count, get_fp()));

	num_tot += num;
	_record += ",";
	_record += to_string(num); } }

    if (num_best < num_tot) die(ERR_INT("bad counts (engine %s)", get_fp()));
    _node.take_action(actionPlay);

    // force declare nyugyoku
    if (_node.get_type().is_interior() && _node.is_nyugyoku())
      _node.take_action(SAux::windecl);
    assert(_node.ok());

    // terminal test
    if (_node.get_type().is_term()) {
      _record += "%";
      _record += _node.get_type().to_str();
      _record += "\n";
      _flag_playing = false;
      return move(_record); }

    engine_out("%s", _startpos.c_str());
    engine_out("go visit");
    return string(""); }

  void engine_out(const char *fmt, ...) noexcept {
    assert(_ofs && Child::ok() && ! Child::is_closed() && fmt);
    char buf[65536];
    va_list argList;
  
    va_start(argList, fmt);
    int nb = vsnprintf(buf, sizeof(buf), fmt, argList);
    va_end(argList);
    if (sizeof(buf) <= static_cast<size_t>(nb) + 1U)
      die(ERR_INT("buffer overrun (engine %d-%d)", _nnet_id, _eid));

    out_log(buf);
    buf[nb]     = '\n';
    buf[nb + 1] = '\0';
    if (!Child::write(buf, strlen(buf))) {
      if (errno == EPIPE) die(ERR_INT("engine %d-%d terminates",
				      _nnet_id, _eid));
      die(ERR_CLL("write")); } }

  const char *get_fp() const noexcept { return _fingerprint.c_str(); }
  
  const string &get_record() const noexcept { return _record; }
  bool is_playing() const noexcept { return _flag_playing; }
  bool is_ready() const noexcept { return _flag_ready; }
  bool is_usistart() const noexcept { return _flag_usistart; }
  uint get_eid() const noexcept { return _eid; }
  uint get_nmove() const noexcept { return _nmove; }
  int get_did() const noexcept { return _device_id; }
  double get_time_average() const noexcept { return _time_average; }
};

PlayManager & PlayManager::get() noexcept {
  static PlayManager instance;
  return instance; }

PlayManager::PlayManager() noexcept : _ngen_records(0) {}
PlayManager::~PlayManager() noexcept {}
void PlayManager::start(const char *cname, const char *dlog,
			const vector<string> &devices_str, uint verbose_eng)
  noexcept {
  assert(cname && dlog);
  if (devices_str.empty()) die(ERR_INT("bad devices"));
  _verbose_eng = verbose_eng;
  _cname.reset_fname(cname);
  _logname.reset_fname(dlog);
  int nnet_id = 0;
  for (const string &s : devices_str) _devices.emplace_back(s, nnet_id++); }

void PlayManager::end() noexcept { _devices.clear(); seq_s.clear(); }

void PlayManager::engine_start(const FNameID &wfname, uint64_t crc64)
  noexcept {
  assert(wfname.ok() && _engines.empty());
  queue<string> e;
  _moves_eid0.swap(e);

  for (Device &d : _devices) d.nnreset(wfname);
  for (Device &d : _devices) d.flush_off();

  int eid = 0;
  for (Device &d : _devices) {
    uint size     = d.get_size_parallel();
    int nnet_id   = d.get_nnet_id();
    int device_id = d.get_device_id();
    char ch       = d.get_id_option_character();
    for (uint u = 0; u < size; ++u)
      _engines.emplace_back(new USIEngine(_cname, ch, device_id, nnet_id,
					  eid++, wfname, crc64, _verbose_eng,
					  _logname)); } }

void PlayManager::engine_terminate() noexcept {
  for (const Device &d : _devices) d.flush_on();
  for (auto &e : _engines) e->engine_out("quit");

  while (!_engines.empty()) {
    Child::wait(1000U);
    for (auto it = _engines.begin(); it != _engines.end(); ) {
      bool flag_err = false;
      bool flag_in  = false;
      if ((*it)->has_line_err()) {
	char line[65536];
	if ((*it)->getline_err(line, sizeof(line)) == 0) flag_err = true;
	else (*it)->out_log(line); }
      
      if ((*it)->has_line_in()) {
	char line[65536];
	if ((*it)->getline_in(line, sizeof(line)) == 0) flag_in = true;
	else (*it)->out_log(line); }
      
      if (flag_err && flag_in) {
	(*it)->close();
	it = _engines.erase(it); }
      else ++it; } }

  for (const Device &d : _devices) d.flush_off(); }


deque<string> PlayManager::manage_play(bool has_conn) noexcept {
  deque<string> recs;

  Child::wait(1000U);
  for (auto &e : _engines) {
    if (e->has_line_err()) {
      char line[65536];
      if (e->getline_err(line, sizeof(line)) == 0)
	die(ERR_INT("An engine (%s) terminates.", e->get_fp()));
      e->out_log(line); }
      
    if (e->has_line_in()) {
      char line[65536];
      if (e->getline_in(line, sizeof(line)) == 0)
	die(ERR_INT("An engine (%s) terminates.", e->get_fp()));
      e->out_log(line);
      string s = e->update(line, _moves_eid0);
      if (!s.empty()) {
	_ngen_records += 1U;
	recs.push_back(move(s)); } }

    if (! e->is_usistart()) e->start_usi();
    if (has_conn && e->is_ready() && ! e->is_playing()) e->start_newgame(); }

  return recs; }

bool PlayManager::get_moves_eid0(string &move) noexcept {
  if (_moves_eid0.empty()) return false;
  move.swap(_moves_eid0.front());
  _moves_eid0.pop();
  return true; }

uint PlayManager::get_eid(uint u) const noexcept {
  assert(u < _engines.size());
  return _engines[u]->get_eid(); }

int PlayManager::get_did(uint u) const noexcept {
  assert(u < _engines.size());
  return _engines[u]->get_did(); }

uint PlayManager::get_nmove(uint u) const noexcept {
  assert(u < _engines.size());
  return _engines[u]->get_nmove(); }

double PlayManager::get_time_average(uint u) const noexcept {
  assert(u < _engines.size());
  return _engines[u]->get_time_average(); }
