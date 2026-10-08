# The native tests build SAM's C files and the C++ codecs together: C++17 goes to
# C++ compiles only, and the sanitizers that build_flags compile with also link.
Import("env")
env.Append(CXXFLAGS=["-std=gnu++17"], LINKFLAGS=["-fsanitize=address,undefined"])
