// 2019 Team AobaZero
// This source code is in the public domain.
#include "err.hpp"
#include "iobase.hpp"
#include "nnet-cpu.hpp"
#include "nnet-ocl.hpp"
#include "nnet-cuda.hpp"
#include "option.hpp"
#include "param.hpp"
#include "shogibase.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <climits>
using std::copy_n;
using std::cout;
using std::cerr;
using std::endl;
using std::getline;
using std::istream;
using std::map;
using std::max;
using std::move;
using std::set;
using std::setw;
using std::string;
using std::stringstream;
using std::terminate;
using std::unique_ptr;
using std::vector;
using std::chrono::steady_clock;
using std::chrono::duration_cast;
using std::chrono::microseconds;
using uint   = unsigned int;
using ushort = unsigned short;
using uchar  = unsigned char;
using namespace ErrAux;
using namespace SAux;

constexpr double epsilon = 1e-2;

static double elapsed  = 0.0;
static uint   nelapsed = 0U;

static uint   value_n       = 0;
static double value_sum_e   = 0.0;
static double value_sum_se  = 0.0;
static double value_max_e   = 0.0;

static uint   policy_n      = 0;
static double policy_sum_e  = 0.0;
static double policy_sum_se = 0.0;
static double policy_max_e  = 0.0;

static int opt_device_id   = -1;
static uint opt_batch_size = 1;
static bool opt_use_half   = false;
static string opt_str_wght;

static double absolute_error(double f1, double f2) noexcept {
  return std::fabs(f1 - f2); }

static int get_options(int argc, const char * const *argv) noexcept {
  assert(0 < argc && argv && argv[0]);
  bool flag_err = false;
  char *endptr;

  while (! flag_err) {
    int opt = Opt::get(argc, argv, "u:b:h");
    if (opt < 0) break;

    long l;
    switch (opt) {
    case 'u': 
      l = strtol(Opt::arg, &endptr, 10);
      if (endptr == Opt::arg || *endptr != '\0' || l < -1 || l == LONG_MAX)
	flag_err = true;
      opt_device_id = static_cast<int>(l);
      break;

    case 'b': 
      l = strtol(Opt::arg, &endptr, 10);
      if (endptr == Opt::arg || *endptr != '\0' || l < 1 || l == LONG_MAX)
	flag_err = true;
      opt_batch_size = static_cast<uint>(l);
      break;

    case 'h': opt_use_half = true;  break;
    default: flag_err = true; break; } }

  if (!flag_err && Opt::ind < argc) {
    opt_str_wght = string(argv[Opt::ind++]);
    return 0; }
  
  cerr << "Usage: " << Opt::cmd
       << " [-u device-id] [-b batch-size] [-h] weight" << endl;
  return -1;
}

class QueueTest {
#if defined(USE_CUDNN)
  NNetCUDA _nnet;
#elif defined(USE_OPENCL)
  NNetOCL _nnet;
#else
  NNetCPU _nnet;
#endif
  unique_ptr<float []> _input;
  unique_ptr<ushort []> _nnmoves;
  unique_ptr<float []> _probs;
  unique_ptr<float []> _values;
  unique_ptr<uint []> _sizes_nnmove;
  unique_ptr<map<ushort, string> []> _tbl_nnmove2str;
  unique_ptr<map<string, double> []> _policy_answers;
  unique_ptr<double []> _value_answers;
  uint _npush, _ntest, _nbatch;

  void test(uint index) const noexcept {
    assert(index < _nbatch);
    
    // test output
    double value_e = absolute_error(_values[index], _value_answers[index]);
    value_n       += 1U;
    value_sum_e   += value_e;
    value_sum_se  += value_e * value_e;
    value_max_e    = max(value_max_e, value_e);
    if (2.0 * epsilon < value_e) {
      cerr << "value1:         " << _values[index] << endl;
      cerr << "value2:         " << _value_answers[index] << endl;
      cerr << "absolute error: " << value_e << endl;
      terminate(); }
  
    map<string, double> policy;
    for (uint u = 0; u < _sizes_nnmove[index]; ++u) {
      ushort us = _nnmoves[index * SAux::maxsize_moves + u];
      const string &str = _tbl_nnmove2str[index].at(us);
      policy[str] = _probs[index * SAux::maxsize_moves + u]; }
	   
    assert(_sizes_nnmove[index] == _policy_answers[index].size());
    auto &&it1 = _policy_answers[index].cbegin();
    auto &&it2 = policy.cbegin();
    while (it1 != _policy_answers[index].cend()
	   && it2 != _policy_answers[index].cend()) {
      double prob1     = (it1++)->second;
      double prob2     = (it2++)->second;
      double policy_e  = absolute_error(prob1, prob2);
      policy_n        += 1U;
      policy_sum_e    += policy_e;
      policy_sum_se   += policy_e * policy_e;
      policy_max_e     = max(policy_max_e, policy_e);
      if (epsilon < policy_e) {
	cerr << "prob1:          " << prob1 << endl;
	cerr << "prob2:          " << prob2 << endl;
	cerr << "absolute error: " << policy_e << endl;
	terminate(); } } }
  
public:
  explicit QueueTest() noexcept : _npush(0), _ntest(0) {}

  void reset(const FName &fname, int device_id, uint nbatch, bool use_half)
    noexcept {
    _input.reset(new float [nbatch * NNAux::size_input]);
    _nnmoves.reset(new ushort [nbatch * SAux::maxsize_moves]);
    _probs.reset(new float [nbatch * SAux::maxsize_moves]);
    _values.reset(new float [nbatch]);
    _sizes_nnmove.reset(new uint [nbatch]);
    _tbl_nnmove2str.reset(new map<ushort, string> [nbatch]);
    _policy_answers.reset(new map<string, double> [nbatch]);
    _value_answers.reset(new double [nbatch]);
    _nbatch = nbatch;
    uint version;
    uint64_t digest;
    NNAux::wght_t wght = NNAux::read(fname, version, digest);
#if defined(USE_CUDNN)
    _nnet.reset(nbatch, wght, device_id, use_half);
#elif defined(USE_OPENCL)
    _nnet.reset(nbatch, wght, device_id, use_half);
#else
    _nnet.reset(nbatch, wght);
#endif
    (void)device_id; (void)use_half; }
  
  void flush() noexcept {
    if (_npush == 0) return;
    steady_clock::time_point start = steady_clock::now();
    _nnet.ff(_npush, _input.get(), _sizes_nnmove.get(), _nnmoves.get(),
	     _probs.get(), _values.get());
    steady_clock::time_point end = steady_clock::now();
    elapsed  += duration_cast<microseconds>(end - start).count();
    nelapsed += 1U;

    for (uint index = 0; index < _npush; ++index) test(index);
    for (uint index = 0; index < _npush; ++index) {
      _ntest += 1U; cout << setw(5) << _ntest; }
    cout << " OK" << endl;
    _npush = 0; }
  
  void push(const float *input, uint size_nnmove, const ushort *nnmoves,
	    double value, const map<string, double> &policy_answers,
	    const map<ushort, string> &nnmove2str) noexcept {
    copy_n(input, NNAux::size_input,
	   &( _input[_npush * NNAux::size_input] ));
    copy_n(nnmoves, SAux::maxsize_moves,
	   &( _nnmoves[_npush * SAux::maxsize_moves] ));
    _sizes_nnmove[_npush]   = size_nnmove;
    _value_answers[_npush]  = value;
    _policy_answers[_npush] = policy_answers;
    _tbl_nnmove2str[_npush] = nnmove2str;
    if (++_npush == _nbatch) flush(); }
};

static QueueTest queue_test;

static void do_test(istream &is) noexcept {

  for (uint uline = 0;;) {
    // read position startpos move...
    string string_line;
    if (! getline(is, string_line)) break;
    uline += 1U;
    
    stringstream ss(string_line);
    string token1, token2, token3;
    ss >> token1 >> token2 >> token3;
    if (token1 != "position" || token2 != "startpos" || token3 != "moves")
      die(ERR_INT("bad line %u", uline));

    NodeNN<Param::maxlen_play> node;
    vector<string> path;
    while (ss >> token1) {
      Action a = node.action_interpret(token1.c_str(), SAux::usi);
      if (! a.is_move()) die(ERR_INT("bad move"));
      node.take_action(a);
      path.emplace_back(move(token1)); }
    
    // read NN input
    uline += 1U;
    if (! getline(is, string_line)) die(ERR_INT("bad line %u", uline));
    ss.clear();
    ss.str(string_line);
    ss >> token1;
    if (token1 != "input") die(ERR_INT("bad line %u", uline));
    vector<double> input_answer;
    double di;
    while (ss >> di) input_answer.push_back(di);

    unique_ptr<float []> input(new float [NNAux::size_input]);
    node.encode_input(input.get());
    if (input_answer.size() != NNAux::size_input)
      die(ERR_INT("bad input size %zu vs %u",
		  input_answer.size(), NNAux::size_input));
    
    for (uint uch = 0; uch < NNAux::nch_input; ++uch)
      for (uint usq = 0; usq < Sq::ok_size; ++usq) {
	double f1 = input_answer[uch * Sq::ok_size + usq];
	double f2 = input[uch * Sq::ok_size + usq];
	if (absolute_error(f1, f2) <= epsilon) continue;
	cerr << "input (answer): " << f1 << endl;
	cerr << "input:          " << f2 << endl;
	cerr << "absolute error: " << absolute_error(f1, f2) << endl;
	terminate(); }

    // read state value
    uline += 1U;
    if (!getline(is, string_line)) die(ERR_INT("bad line %u", uline));
    ss.clear();
    ss.str(string_line);
    ss >> token1;
    if (token1 != "value") die(ERR_INT("bad line %u", uline));
    double value_answer;
    ss >> value_answer;

    // read action probabilities
    uline += 1U;
    if (!getline(is, string_line)) die(ERR_INT("bad line %u", uline));
    ss.clear();
    ss.str(string_line);
    ss >> token1;
    if (token1 != "policy") die(ERR_INT("bad line %u", uline));

    map<string, double> policy_answer;
    while (ss >> token1 >> di) policy_answer.emplace(move(token1), di);

    // test genmove
    MoveSet<Param::maxlen_play> ms;
    ms.gen_all(node);
    assert(ms.ok());
    
    set<string> set1, set2;
    for (auto &f : policy_answer)        set1.insert(f.first);
    for (uint u = 0; u < ms.size(); ++u) set2.emplace(ms[u].to_str(SAux::usi));
    if (set1 != set2) {
      cerr << "position startpos moves";
      for (auto &s : path) cerr << " " << s;
      cerr << "\n" << node.to_str();
      
      auto &&it1 = set1.cbegin();
      auto &&it2 = set2.cbegin();
      while (it1 != set1.cend() || it2 != set2.cend()) {
	if (it1 == set1.cend()) cerr << "      ";
	else cerr << std::setw(6) << std::left << *it1++;
	
	if (it2 != set2.cend()) cerr << std::setw(6) << std::left << *it2++;
	cerr << endl; }
      die(ERR_INT("bad move generation")); }
    
    // read END
    uline += 1U;
    if (!getline(is, string_line)) die(ERR_INT("bad line %u", uline));
    if (string_line != "END") die(ERR_INT("bad line %u", uline));
    if (ms.size() == 0) continue;

    // push target position
    map<ushort, string> nnmove2str;
    ushort nnmoves2[SAux::maxsize_moves];
    for (uint u = 0; u < ms.size(); ++u) {
      nnmoves2[u] = NNAux::encode_nnmove(ms[u], node.get_turn());
      nnmove2str[nnmoves2[u]] = ms[u].to_str(SAux::usi); }
    
    if (node.get_turn() == white) value_answer = - value_answer;
    
    queue_test.push(input.get(), ms.size(), nnmoves2, value_answer,
		    policy_answer, nnmove2str); } }

int main(int argc, char **argv) {
  if (get_options(argc, argv) < 0) return 1;
  queue_test.reset(FName(opt_str_wght.c_str()), opt_device_id, opt_batch_size,
		   opt_use_half);
  do_test(std::cin);
  queue_test.flush();

  if (0 < nelapsed) {
    cout << "Average Time: "
      << 0.001 * elapsed / static_cast<double>(nelapsed)
	 << "ms" << endl; }
  
  if (0 < value_n) {
    double factor = 1.0 / static_cast<double>(value_n);
    cout << "Value (ABSOLUTE, " << value_n << " samples)\n";
    cout << "  - MaxE: " << value_max_e << "\n";
    cout << "  - ME:   " << value_sum_e * factor << "\n";
    cout << "  - RMSE: " << std::sqrt(value_sum_se * factor) << "\n\n"; }
    
  if (0 < policy_n) {
    double factor = 1.0 / static_cast<double>(policy_n);
    cout << "Policy (ABSOLUTE, " << policy_n << " samples)\n";
    cout << "  - MaxE: " << policy_max_e << "\n";
    cout << "  - ME:   " << policy_sum_e * factor << "\n";
    cout << "  - RMSE: " << std::sqrt(policy_sum_se * factor) << "\n\n"; }
  
  return 0; }
