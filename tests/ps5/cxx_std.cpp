// PS5 compat: C++: exceptions (incl. unwinding through frames, rethrow, nested), RTTI, iostream, STL, std::thread, mutex.
#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>
using namespace std;
struct Base { virtual ~Base() {} virtual string name() { return "Base"; } };
struct Derived : Base { string name() override { return "Derived"; } };
struct Guard { string n; Guard(string s) : n(s) {} ~Guard() { cout << "~Guard " << n << endl; } };
static void thrower(int d) { Guard g("d" + to_string(d)); if (d == 0) throw runtime_error("boom"); thrower(d - 1); }
int main() {
    cout << "Hello, C++" << endl;
    try { thrower(2); } catch (const exception &e) { cout << "caught: " << e.what() << endl; }
    try { try { throw 7; } catch (int i) { cout << "inner " << i << endl; throw string("re"); } }
    catch (const string &s) { cout << "outer " << s << endl; }
    try { vector<int> v; v.at(3); } catch (const out_of_range &) { cout << "out_of_range" << endl; }
    try { throw Derived(); } catch (Base &b) { cout << "polymorphic catch: " << b.name() << " " << typeid(b).name() << endl; }
    unique_ptr<Base> p(new Derived); cout << "dyncast " << (dynamic_cast<Derived *>(p.get()) != nullptr) << endl;
    vector<int> v = {5, 3, 9, 1, 7}; sort(v.begin(), v.end());
    for (int n : v) cout << n << ' '; cout << endl;
    map<string, int> m; m["b"] = 2; m["a"] = 1; for (auto &kv : m) cout << kv.first << '=' << kv.second << ' '; cout << endl;
    ostringstream os; os << 3.5 << ' ' << hex << 255 << ' ' << boolalpha << true; cout << os.str() << endl;
    mutex mu; int total = 0; vector<thread> ts;
    for (int i = 0; i < 4; i++) ts.emplace_back([&, i] { for (int k = 0; k < 1000; k++) { lock_guard<mutex> l(mu); total += i; } });
    for (auto &t : ts) t.join();
    cout << "threads total=" << total << endl;
    thread t([] { try { throw 99; } catch (int e) { cout << "thread caught " << e << endl; } }); t.join();
    shared_ptr<int> sp = make_shared<int>(42); cout << *sp << " use_count=" << sp.use_count() << endl;
    return 0;
}
