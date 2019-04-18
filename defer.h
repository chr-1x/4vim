// Some C++ Bullshit to get a defer statement
#ifndef __DEFER_H__
#define __DEFER_H__

#include <functional>

using defer_func = std::function<void()>;
struct _Defer {
    defer_func the_func;
    _Defer(defer_func func) : the_func(func) {}
    ~_Defer() { the_func(); }
};
#define defer(s) _Defer defer##__LINE__([&] { s; })

#endif  // __DEFER_H__
