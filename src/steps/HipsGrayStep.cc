// Copyright (C) 2011  Dmitri Nikulin
// Copyright (C) 2011  Monash University
//
// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use,
// copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following
// conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.

#include "cvd-cl/steps/HipsGrayStep.hh"
#include "kernels/hips-gray.hh"

namespace CVD {
namespace CL  {

// Create (0,0) offset.
cl_int2 static const offset00 = {{0, 0}};

HipsGrayStep::HipsGrayStep(GrayImageState & i_image, PointListState & i_points, HipsListState & o_hips) :
    WorkerStep (i_image.worker),
    i_image    (i_image),
    i_points   (i_points),
    o_hips     (o_hips)
{
    worker.compile(&program, &kernel, OCL_HIPS_GRAY, "hips_gray");
}

HipsGrayStep::~HipsGrayStep() {
    // Do nothing.
}

void HipsGrayStep::execute() {
    // Assign kernel parameters.
    kernel.setArg(0, i_image.image);
    kernel.setArg(1, i_points.buffer);
    kernel.setArg(2, o_hips.buffer);
    kernel.setArg(3, offset00);

    // Read number of input points.
    size_t const np = i_points.getCount();

    // Round down number of input points.
    size_t const np_64 = ((np / 64) * 64);

    // Reset number of output points.
    o_hips.setCount(np_64);

    // Queue kernel with global size set to number of input points.
    worker.queue.enqueueNDRangeKernel(kernel, cl::NullRange, cl::NDRange(np_64), cl::NDRange(64));
}

} // namespace CL
} // namespace CVD
