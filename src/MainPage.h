/** \file
 * This file only exists to contain the front-page of the documentation
 */

/** \mainpage Halide
 *
 * Halide is a programming language designed to make it easier to
 * write high-performance image processing code on modern
 * machines. Its front end is embedded in C++. Compiler
 * targets include x86/SSE, ARM v7/NEON, CUDA, Native Client, and
 * OpenCL.
 *
 * You build a Halide program by writing C++ code using objects of
 * type \ref Halide::Var, \ref Halide::Expr, and \ref Halide::Func,
 * and then calling \ref Halide::Func::compile_to_file to generate an
 * object file and header (good for deploying large routines), or
 * calling \ref Halide::Func::realize to JIT-compile and run the
 * pipeline immediately (good for testing small routines).
 *
 * To learn Halide, we recommend you start with the <a href=examples.html>tutorials</a>.
 *
 * You can also look in the test folder for many small examples that
 * use Halide's various features, and in the apps folder for some
 * larger examples that statically compile halide pipelines. In
 * particular check out local_laplacian, bilateral_grid, and
 * interpolate.
 *
 * Below are links to the documentation for the important classes in Halide.
 *
 * For defining, scheduling, and evaluating basic pipelines:
 *
 * Halide::Func, Halide::Var
 *
 * Our image data type:
 *
 * Halide::Image
 *
 * For passing around and reusing halide expressions:
 *
 * Halide::Expr
 *
 * For representing scalar and image parameters to pipelines:
 *
 * Halide::Param, Halide::ImageParam
 *
 * For writing functions that reduce or scatter over some domain:
 *
 * Halide::RDom
 *
 * For writing and evaluating functions that return multiple values:
 *
 * Halide::Tuple, Halide::Realization
 *
 */

/**
 * \example tutorial/lesson_01.cpp
 * \example tutorial/lesson_02.cpp
 * \example tutorial/lesson_03.cpp
 * \example tutorial/lesson_04.cpp
 */
