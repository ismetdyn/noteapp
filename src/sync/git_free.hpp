#pragma once

namespace noteapp::sync {

// libgit2 nesnelerinin unique_ptr silicisi: std::unique_ptr<git_x, GitFree<git_x_free>>.
template <auto Free> struct GitFree {
    template <class T> void operator()(T* ptr) const { Free(ptr); }
};

}  // namespace noteapp::sync
