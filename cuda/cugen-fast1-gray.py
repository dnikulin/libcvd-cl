#!/usr/bin/env python
#
# Copyright (C) 2011  Dmitri Nikulin
# Copyright (C) 2011  Monash University
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

# 2D offsets from "corner" candidate pixel.
# Corresponds to SUBSET OF lines 17-32 in fast_9_detect.cxx
OFFSETS = [
    ( 0,  3),
    ( 3,  0),
    ( 0, -3),
    (-3,  0),
]

for (shift, (x, y)) in enumerate(OFFSETS, 1):
    print ("    int const p%02d = tex2D(testImage, x + %2d, y + %2d).x;" % (shift, x, y))
print

print "    // Check the absolute difference of each circle pixel."
for (shift, _) in enumerate(OFFSETS, 1):
    print ("    int const d%02d = (abs(p%02d - p00) > FAST_THRESH);" % (shift, shift))
print

print "    // Check if any two adjacent circle pixels have a high absolute difference."
print "    int const isCorner = ("
print " ||\n".join([
    ("        (d%02d && d%02d)" % (shift, (shift % len(OFFSETS)) + 1))
    for (shift, _) in enumerate(OFFSETS, 1)
])
print "    );"
