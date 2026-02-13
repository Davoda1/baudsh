#ifdef _WIN32
    #include "baudsh_win.hpp"
    int baudsh_windows_main(int argc, char** argv);
#else
    #include "baudsh_posix_features.hpp"
    int baudsh_posix_main(int argc, char** argv);
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    return baudsh_windows_main(argc, argv);
#else
    return baudsh_posix_main(argc, argv);
#endif
}