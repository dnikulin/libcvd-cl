#!/usr/bin/env python
#
# Copyright (C) 2011  Dmitri Nikulin, Monash University
#
# Permission is hereby granted, free of charge, to any person
# obtaining a copy of this software and associated documentation
# files (the "Software"), to deal in the Software without
# restriction, including without limitation the rights to use,
# copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following
# conditions:
#
# The above copyright notice and this permission notice shall be
# included in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
# OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
# HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
# WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

print """// Copyright (C) 2011  Dmitri Nikulin, Monash University
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

// Enable OpenCL 32-bit integer atomic functions.
#pragma OPENCL EXTENSION cl_khr_global_int32_base_atomics : enable

// Parallel bit counting magic adapted from
// http://graphics.stanford.edu/~seander/bithacks.html#CountBitsSetParallel
uint bitcount16(uint16 v) {
    v = (v - ((v >> 1) & 0x55555555));
    v = ((v & 0x33333333) + ((v >> 2) & 0x33333333));
    v = ((((v + (v >> 4)) & 0xF0F0F0F) * 0x1010101) >> 24);

    // Fold together in halves.
    uint8 v8 = (v.lo + v.hi);
    uint4 v4 = (v8.lo + v8.hi);
    return (v4.x + v4.y + v4.z + v4.w);
}

uint error(uint16 t, uint16 r) {
    return bitcount16(t & ~r);
}

kernel void hips_tree_find(
    read_only image2d_t     hashesR,  // R (forest of descriptors, as above)
    read_only image2d_t     indices,  // Original index of each hash in R, defined only for leaves.
    global   ulong8 const * hashesT,  // T (list of descriptors)
    global   uint2        * matches,  // Pairs of indices into hashes1 and hashes2.
    global   uint         * imatch,   // Output number of hash1 matches.
             uint           nmatch    // Maximum number of matches.
) {

    // Prepare a suitable OpenCL image sampler.
    sampler_t const sampler = CLK_ADDRESS_CLAMP | CLK_FILTER_NEAREST;

    // Use global work item in dimension 0 for hashT index.
    uint   const ihashT  = get_global_id(0);
    ulong8 const  hashT0 = hashesT[ihashT];

    // Use global work item in dimension 1 for rotation index.
    uint   const ishift  = get_global_id(1);
    uint   const lshift  = (ishift * 4);
    uint   const rshift  = (64 - lshift);

    // Rotate and cast descriptor.
    uint16 const  hashT  = as_uint16((hashT0 >> rshift) | (hashT0 << lshift));

    // Loop over pre-roots.
    #pragma unroll
    for (uint iroot = 0; iroot < TREE_PRE_ROOTS; iroot++) {
        // Start traversal at root.
        uint icell = (iroot + TREE_PRE_ROOTS - 1);
        uint last  = 10000;

        // Recurse within available tree levels..
        #pragma unroll
        for (uint idepth = 0; idepth < TREE_LEVELS; idepth++) {
            // Calculate positions of both children.
            uint const icell0 = (icell  * 2);
            uint const icell1 = (icell0 + 1);
            uint const icell2 = (icell0 + 2);

            // Correct for tree truncation.
            uint const icell10 = (icell1 - TREE_DROP_NODES);
            uint const icell20 = (icell2 - TREE_DROP_NODES);

            // Read integers for both children.
            uint16 hashR1;
            hashR1.lo.lo = read_imageui(hashesR, sampler, (int2)(0, icell10));
            hashR1.lo.hi = read_imageui(hashesR, sampler, (int2)(1, icell10));
            hashR1.hi.lo = read_imageui(hashesR, sampler, (int2)(2, icell10));
            hashR1.hi.hi = read_imageui(hashesR, sampler, (int2)(3, icell10));

            uint16 hashR2;
            hashR2.lo.lo = read_imageui(hashesR, sampler, (int2)(0, icell20));
            hashR2.lo.hi = read_imageui(hashesR, sampler, (int2)(1, icell20));
            hashR2.hi.lo = read_imageui(hashesR, sampler, (int2)(2, icell20));
            hashR2.hi.hi = read_imageui(hashesR, sampler, (int2)(3, icell20));

            // Calculate errors for both children.
            uint const err1 = error(hashT, hashR1);
            uint const err2 = error(hashT, hashR2);

            // Determine lower error.
            last = min(err1, err2);

            // Keep child with lower error.
            icell = select(icell1, icell2, err1 > err2);
        }

        // Record match if within error threshold.
        if (last <= HIPS_MAX_ERROR) {
            uint const i = atom_inc(imatch);
            if (i < nmatch) {
                uint const index = read_imageui(indices, sampler, (int2)(0, icell - TREE_LEAF0)).x;

                // Store pair against original index.
                matches[i] = (uint2)(index, ihashT);
            }
        }
    }
}"""
