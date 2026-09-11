# Standalone benchmark, not a GoogleTest binary: it needs to run for several
# real seconds with two live threads, which is a different job than a unit
# test and doesn't belong in `ctest`.
add_executable(ridgeline_bench ring_buffer_bench.cc)
target_link_libraries(ridgeline_bench PRIVATE ridgeline_core ridgeline_warnings Threads::Threads)
# Always optimized, even in a Debug configuration of the rest of the tree —
# an unoptimized benchmark measures the compiler, not the algorithm.
target_compile_options(ridgeline_bench PRIVATE -O3)
