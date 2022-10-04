#ifndef HALIDE_PYTHON_BINDINGS_PYDERIVATIVE_H
#define HALIDE_PYTHON_BINDINGS_PYDERIVATIVE_H

#include "PyHalide.h"

namespace Halide {
namespace PythonBindings {

void define_derivative(py::module_ &m);

}  // namespace PythonBindings
}  // namespace Halide

#endif  // HALIDE_PYTHON_BINDINGS_PYDERIVATIVE_H
