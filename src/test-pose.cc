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

#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include <cvd/camera.h>
#include <cvd/fast_corner.h>
#include <cvd/gl_helpers.h>
#include <cvd/image_io.h>
#include <cvd/videodisplay.h>

#include <cvd-cl/steps/PreFastGrayStep.hh>
#include <cvd-cl/steps/ClipDepthStep.hh>
#include <cvd-cl/steps/FastGrayStep.hh>
#include <cvd-cl/steps/HipsBlendGrayStep.hh>
#include <cvd-cl/steps/HipsMakeTreeStep.hh>
#include <cvd-cl/steps/HipsTreeFindStep.hh>
#include <cvd-cl/steps/HipsFindStep.hh>
#include <cvd-cl/steps/HipsClipStep.hh>
#include <cvd-cl/steps/ToUvqUvStep.hh>
#include <cvd-cl/steps/MixUvqUvStep.hh>
#include <cvd-cl/steps/PoseUvqWlsStep.hh>
#include <cvd-cl/steps/CholeskyStep.hh>
#include <cvd-cl/steps/MatIdentStep.hh>
#include <cvd-cl/steps/MatMulStep.hh>
#include <cvd-cl/steps/SE3ExpStep.hh>
#include <cvd-cl/steps/SE3ScoreStep.hh>
#include <cvd-cl/steps/SE3Run1Step.hh>

#include <boost/program_options.hpp>

// Typedefs for Blitz-based image format.
typedef blitz::Array<cl_uchar, 3>  GrayImage;
typedef blitz::Array<cl_float, 3> DepthImage;

// Size constants.
size_t const static KiB = 1024;
size_t const static MiB = KiB * KiB;

// Maximum corners processed.
size_t const static ncorners = 2048;

// Number of hypotheses to generate.
size_t const static nhypos   = 8192;

struct options {
    cl_int fast_threshold;
    cl_int fast_ring;
    cl_int hips_maxbits;
    cl_int hips_maxerr;
    cl_int hips_blendsize;
    cl_int hips_leaves;
    cl_int hips_levels;
    bool   hips_rotate;
};

static void readCamera(Camera::Linear * camera, char const * path) {
    // Open parameter file.
    std::ifstream file(path);
    file.exceptions(~std::ios::goodbit);

    // Consume parameters.
    camera->load(file);

    // Close file.
    file.close();
}

static void learnCamera(Camera::Linear const & cvd_camera, CVD::CL::CameraState & camera) {
    // Loop over all coordinates.
    for (int y = 0; y < int(camera.ny); y++) {
        for (int x = 0; x < int(camera.nx); x++) {
            // Construct (x, y) vector.
            // NB: The camera size does not match the image size,
            // however, the offset is 0, so use (x, y) directly.
            TooN::Vector<2> const xy = TooN::makeVector(x, y);

            // Translate from (x, y) to (u, v).
            TooN::Vector<2> const uv = cvd_camera.unproject(xy);

            // Record (u, v) pair.
            camera.udata(y, x, 0) = uv[0];
            camera.vdata(y, x, 0) = uv[1];
        }
    }
}

static void readRGBD(
     GrayImage & colour,
    DepthImage & depth,
    char const * path
) {

    // Open file, enabling exceptions.
    std::ifstream file;
    file.exceptions(~std::ios_base::goodbit);
    file.open(path, std::ios::in | std::ios::binary);

    int nx = 0;
    int ny = 0;

    // Read image size.
    file >> nx;
    file >> ny;

    assert(nx > 0);
    assert(ny > 0);

    // Allocate images of given size.
    colour.resize(ny, nx, 4);
     depth.resize(ny, nx, 1);

    // Reset image data.
    colour = 0;
     depth = 0;

    for (int y = 0; y < ny; y++) {
        for (int x = 0; x < nx; x++) {
            cl_uint r = 0;
            cl_uint g = 0;
            cl_uint b = 0;
            cl_uint d = 0;

            file >> r;
            file >> g;
            file >> b;
            file >> d;

            assert(r <= 0xFF);
            assert(g <= 0xFF);
            assert(b <= 0xFF);
            assert(d <= 0xFFFF);

            colour(y, x, blitz::Range::all()) = ((r + g + b) / 3);

             depth(y, x, blitz::Range::all()) = d;
        }
    }

    // Close file.
    file.close();
}

struct stage1input {
    GrayImage   g1image;
    GrayImage   g2image;
    DepthImage  d1image;
    DepthImage  d2image;
    options     opts;
};

static void testPipeline(
    cl::Device        & device,
    stage1input const & input
) {
    // Refer to inputs.
    options const & opts = input.opts;

    // Extract image dimensions.
    int const ny  = input.g1image.length(0);
    int const nx  = input.g1image.length(1);
    int const nxy = nx * ny;

    // Create OpenCL worker.
    CVD::CL::Worker          worker      (device);

    // Create FAST and HIPS states.
    CVD::CL::RichImageState  imageNeat   (worker, ny, nx);
    CVD::CL::PointListState  corners1    (worker, nxy);
    CVD::CL::PointListState  corners2    (worker, nxy);
    CVD::CL::PointListState  corners3    (worker, nxy);

    // Create states specific to image1 (colour + depth).
    CVD::CL::PointListState  im1corners  (worker, ncorners);
    CVD::CL::HipsListState   im1hips     (worker, ncorners);
    CVD::CL::GrayImageState  im1depth    (worker, ny, nx);

    // Create states specific to image2 (colour only).
    CVD::CL::PointListState  im2corners  (worker, ncorners);
    CVD::CL::HipsListState   im2hips     (worker, ncorners);

    // Create camera translation states.
    CVD::CL::CameraState     camera      (worker, ny, nx);

    // Create state for HIPS tree based on stage 1.
    CVD::CL::HipsTreeState   im1tree     (worker, opts.hips_leaves, opts.hips_levels);

    // Create states for RANSAC.
    // None of these require images, so they may be used on CPU.
    CVD::CL::PointListState  matches     (worker, ncorners);
    CVD::CL::UvqUvState<1>     uvquv       (worker, ncorners);
    CVD::CL::UvqUvState<3>     uvquv_mix   (worker, nhypos);
    CVD::CL::MatrixState<4, 4> hypo_m      (worker, nhypos);
    CVD::CL::MatrixState<6, 6> hypo_a      (worker, nhypos);
    CVD::CL::MatrixState<6, 1> hypo_b      (worker, nhypos);
    CVD::CL::MatrixState<6, 1> hypo_x      (worker, nhypos);
    CVD::CL::MatrixState<4, 4> hypo_cam    (worker, nhypos);
    CVD::CL::FloatListState  hypo_scores (worker, nhypos);
    CVD::CL::CountState      hypo_best   (worker, nhypos);
    CVD::CL::Float2ListState test_uvs    (worker, ncorners);

    // Create steps specific to image1.
    CVD::CL::PreFastGrayStep runPreFast1 (imageNeat, corners1, opts.fast_threshold);
    CVD::CL::ClipDepthStep   runClip1    (camera.qmap,  corners1, corners2);
    CVD::CL::FastGrayStep    runFast1    (imageNeat, corners2, im1corners, opts.fast_threshold, opts.fast_ring);
    CVD::CL::HipsBlendGrayStep    runHips1    (imageNeat,                             im1corners, im1hips, opts.hips_blendsize);
    CVD::CL::HipsClipStep    runHipsClip1(im1hips, opts.hips_maxbits);

    // Create steps specific to image2.
    CVD::CL::PreFastGrayStep runPreFast2 (imageNeat, corners1, opts.fast_threshold);
    CVD::CL::ClipDepthStep   runClip2    (camera.qmap,  corners1, corners2);
    CVD::CL::FastGrayStep    runFast2    (imageNeat, corners2, im2corners, opts.fast_threshold, opts.fast_ring);
    CVD::CL::HipsBlendGrayStep    runHips2    (imageNeat,                                                  im2corners, im2hips, 1);
    CVD::CL::HipsClipStep    runHipsClip2(im2hips, opts.hips_maxbits);

    // Create step for HIPS tree based on stage 1.
    CVD::CL::HipsMakeTreeStep runTree1   (im1hips, im1tree);

    // Create steps for RANSAC.
    CVD::CL::HipsTreeFindStep runMatch    (im1tree, im2hips, matches, opts.hips_maxerr, opts.hips_rotate);
    CVD::CL::ToUvqUvStep     runToUvqUv  (camera, im1corners, im2corners, matches, uvquv);
    CVD::CL::MixUvqUvStep    runMix      (uvquv, uvquv_mix);
    CVD::CL::MatIdentStep<4> runIdent    (hypo_m);
    CVD::CL::PoseUvqWlsStep  runWls      (uvquv_mix, hypo_m, hypo_a, hypo_b);
    CVD::CL::CholeskyStep<6> runCholesky (hypo_a, hypo_b, hypo_x);
    CVD::CL::SE3ExpStep      runSe3Exp   (hypo_x, hypo_cam);
    CVD::CL::MatMulStep<4>   runMul      (hypo_cam, hypo_m);
    CVD::CL::SE3ScoreStep    runSe3Score (uvquv, hypo_cam, hypo_scores);
    CVD::CL::SE3Run1Step     runSe3One   (uvquv, hypo_cam, hypo_best, test_uvs);



    // Populate camera states.
    Camera::Linear cvdcamera;
    readCamera(&cvdcamera, "./etc/kinect.conf");
    learnCamera(cvdcamera, camera);

    // Write image 1 to device.
    int64_t const timeCopy1 = 0;
    CVD::CL::setImage(imageNeat, input.g1image);
    camera.qdata = input.d1image;
    camera.copyToWorker();
    worker.finish();

    // Run image 1 pipeline.
    int64_t const timePreFast1 = runPreFast1.measure();
    size_t const ncull1 = corners1.getCount();
    int64_t const timeClip1 = runClip1.measure();
    size_t const nclip1 = corners2.getCount();
    int64_t const timeFast1 = runFast1.measure();
    size_t const nfast1 = im1corners.getCount();
    size_t const nbest1 = im1corners.getCount();
    int64_t const timeHips1 = runHips1.measure();
    int64_t const timeHClip1 = runHipsClip1.measure();

    // Write image 2 to device.
    int64_t const timeCopy2 = 0;
    CVD::CL::setImage(imageNeat, input.g2image);
    camera.qdata = input.d2image;
    camera.copyToWorker();
    worker.finish();

    // Run image 2 pipeline.
    int64_t const timePreFast2 = runPreFast2.measure();
    size_t const ncull2 = corners1.getCount();
    int64_t const timeClip2 = runClip2.measure();
    size_t const nclip2 = corners2.getCount();
    int64_t const timeFast2 = runFast2.measure();
    size_t const nfast2 = im2corners.getCount();
    size_t const nbest2 = im2corners.getCount();
    int64_t const timeHips2 = runHips2.measure();
    int64_t const timeHClip2 = runHipsClip2.measure();

    // Finish any outstanding work.
    worker.finish();

    std::cerr << std::endl;
    std::cerr << std::setw(8) << nxy    << std::setw(8) << nxy    << " corner candidates in image" << std::endl;
    std::cerr << std::setw(8) << ncull1 << std::setw(8) << ncull2 << " corners after culling" << std::endl;
    std::cerr << std::setw(8) << nclip1 << std::setw(8) << nclip2 << " corners after depth" << std::endl;
    std::cerr << std::setw(8) << nfast1 << std::setw(8) << nfast2 << " corners after FAST" << std::endl;
    std::cerr << std::endl;
    std::cerr << std::setw(8) << timeCopy1       << std::setw(8) << timeCopy2      << " us writing image" << std::endl;
    std::cerr << std::setw(8) << timePreFast1    << std::setw(8) << timePreFast2   << " us culling corners" << std::endl;
    std::cerr << std::setw(8) << timeClip1       << std::setw(8) << timeClip2      << " us filtering by depth" << std::endl;
    std::cerr << std::setw(8) << timeFast1       << std::setw(8) << timeFast2      << " us running FAST" << std::endl;
    std::cerr << std::setw(8) << timeHips1       << std::setw(8) << timeHips2      << " us making HIPS" << std::endl;
    std::cerr << std::setw(8) << timeHClip1      << std::setw(8) << timeHClip2     << " us clipping HIPS" << std::endl;
    std::cerr << std::endl;

    // Read out final corner lists.
    std::vector<cl_int2>   points1;
    std::vector<cl_int2>   points2;
    im1corners.get(&points1);
    im2corners.get(&points2);

    boost::system_time const t1 = boost::get_system_time();


    // Run HIPS tree step.
    int64_t const timeTree     = runTree1.measure();

    // Run RANSAC steps.
    int64_t const timeMatch    = runMatch.measure();
    size_t  const nmatch       = matches.getCount();
    int64_t const timeToUvqUv  = runToUvqUv.measure();
    int64_t const timeMix      = runMix.measure();
    int64_t const timeIdent    = runIdent.measure();


    std::cerr << std::setw(8) << timeTree        << " us making HIPS tree" << std::endl;
    std::cerr << std::setw(8) << timeMatch       << " us finding HIPS matches" << std::endl;
    std::cerr << std::setw(8) << timeToUvqUv     << " us converting matches to ((u,v,q),(u,v))" << std::endl;
    std::cerr << std::setw(8) << timeMix         << " us selecting matches for 3-point attempts" << std::endl;
    std::cerr << std::setw(8) << timeIdent       << " us assigning identity matrix" << std::endl;
    std::cerr << std::endl;
    std::cerr << std::setw(8) << nmatch << " HIPS matches" << std::endl;
    std::cerr << std::endl;

    for (int i = 0; i < 10; i++) {
        int64_t const timeWls      = runWls.measure();
        int64_t const timeCholesky = runCholesky.measure();
        int64_t const timeSe3Exp   = runSe3Exp.measure();
        int64_t const timeMul      = runMul.measure(1); // Do not repeat!

        std::cerr << std::setw(8) << timeWls         << " us differentiating matrix" << std::endl;
        std::cerr << std::setw(8) << timeCholesky    << " us decomposing matrix and back-substituting vector" << std::endl;
        std::cerr << std::setw(8) << timeSe3Exp      << " us exponentiating matrix" << std::endl;
        std::cerr << std::setw(8) << timeMul         << " us multiplying matrix" << std::endl;
        std::cerr << std::endl;
    }

    int64_t const timeSe3Score = runSe3Score.measure();
    std::cerr << std::setw(8) << timeSe3Score    << " us scoring matrix" << std::endl;
    std::cerr << std::endl;

    // Read out score list.
    std::vector<cl_float> hyposcores;
    hypo_scores.get(&hyposcores);

    /* Calculate score statistics. */ {
        cl_float total = 0;
        cl_float best  = 0;
        cl_int   non0  = 0;
        cl_int   ibest = 0;

        boost::system_time const t1 = boost::get_system_time();

        for (size_t i = 0; i < hyposcores.size(); i++) {
            cl_float const score = hyposcores.at(i);

            total += score;
            non0  += (score > 0);

            if (score > best) {
                best  = score;
                ibest = i;
            }
        }

        cl_float avg = (total / hyposcores.size());

        boost::system_time const t2 = boost::get_system_time();

        std::cerr << std::setw(8) << non0  << " non-zero scores" << std::endl;
        std::cerr << std::setw(8) << total << " total score" << std::endl;
        std::cerr << std::setw(8) << avg   << " average score" << std::endl;
        std::cerr << std::setw(8) << best  << " best score" << std::endl;
        std::cerr << std::setw(8) << ibest << " best matrix index" << std::endl;

        int64_t const bestTime = ((t2 - t1).total_microseconds() / 1);

        std::cerr << std::setw(8) << bestTime         << " us finding best matrix" << std::endl;

        // Assign and run best matrix.
        worker.finish();
        hypo_best.setCount(ibest);
        runSe3One.measure();
    }

    // Read out pair lists.
    std::vector<cl_int2>   pairs;
    matches.get(&pairs);

    // Read out transformed coordinate list.
    std::vector<cl_float2> uv2s;
    test_uvs.get(&uv2s);

    boost::system_time const t2 = boost::get_system_time();

    int64_t const approxTime = ((t2 - t1).total_microseconds() / 10);

    std::cerr << std::setw(8) << approxTime      << " us approximate total" << std::endl;

    CVD::ImageRef const size2(nx * 2, ny * 2);
    CVD::VideoDisplay window(size2);

    CVD::CL::glDrawPixelsRGBA(input.g1image);
    ::glRasterPos2i(nx,  0);
    CVD::CL::glDrawPixelsRGBA(input.g2image);
    ::glRasterPos2i( 0, ny);
    CVD::CL::glDrawPixelsRGBA(input.g1image);
    ::glRasterPos2i(nx, ny);
    CVD::CL::glDrawPixelsRGBA(input.g2image);

    glBegin(GL_LINES);
    for (size_t ip = 0; ip < pairs.size(); ip++) {
        try {
            cl_int2         const pair = pairs.at(ip);

            cl_int2         const xy1  = points1.at(pair.x);
            cl_int2         const xy2  = points2.at(pair.y);

            cl_float2       const uv3  = uv2s.at(ip);
            TooN::Vector<2> const uv3t = TooN::makeVector(uv3.x, uv3.y);
            TooN::Vector<2> const xy3t = cvdcamera.project(uv3t);
            cl_int2         const xy3  = {{cl_int(xy3t[0]), cl_int(xy3t[1])}};

            // Red: RANSAC match.
            glColor3f(1, 0, 0);
            glVertex2i(xy1.x,      xy1.y + ny);
            glVertex2i(xy3.x + nx, xy3.y + ny);
        } catch (...) {
            std::cerr << "Bad pair " << ip << " of " << pairs.size() << std::endl;
        }
    }
    glEnd();
    glFlush();

    // Green: corners.
    glColor3f(0, 1, 0);
    glBegin(GL_POINTS);
    for (size_t i = 0; i < points1.size(); i++) {
        cl_int2 const & xy = points1.at(i);
        glVertex2i(xy.x, xy.y);
        glVertex2i(xy.x, xy.y + ny);
    }
    for (size_t i = 0; i < points2.size(); i++) {
        cl_int2 const & xy = points2.at(i);
        glVertex2i(xy.x + nx, xy.y);
        glVertex2i(xy.x + nx, xy.y + ny);
    }
    glEnd();
    glFlush();

    sleep(5);
}

int main(int argc, char **argv) {
    namespace po = boost::program_options;

    options opts;
    ::memset(&opts, 0, sizeof(opts));

    std::string path1;
    std::string path2;

    po::options_description opt;
    opt.add_options()
        ("help,h",
            "Produce this help message")
        ("path1,f1",
            po::value(&path1)->default_value("images/kinect001.txt"),
            "Plaintext RGBD image 1")
        ("path2,f2",
            po::value(&path2)->default_value("images/kinect002.txt"),
            "Plaintext RGBD image 2")
        ("fast-thresh,t",
            po::value(&opts.fast_threshold)->default_value(40),
            "FAST absolute difference threshold")
        ("fast-ring,r",
            po::value(&opts.fast_ring)->default_value(9),
            "FAST ring size")
        ("hips-blend-size,B",
            po::value(&opts.hips_blendsize)->default_value(CVD::CL::HipsBlend5),
            "HIPS blend size ( 1 | 5 | 9 )")
        ("hips-max-bits,b",
            po::value(&opts.hips_maxbits)->default_value(150),
            "HIPS maximum 1-bits per descriptor")
        ("hips-max-error,e",
            po::value(&opts.hips_maxerr)->default_value(3),
            "HIPS maximum error bits per match")
        ("hips-tree-leaves,l",
            po::value(&opts.hips_leaves)->default_value(512),
            "HIPS descriptor tree leaves")
        ("hips-tree-levels,L",
            po::value(&opts.hips_levels)->default_value(5),
            "HIPS descriptor tree levels")
        ("no-rotate,R",
            "Do not rotate HIPS descriptors")
        ;

    po::positional_options_description pos;
    pos.add("path1", 1);
    pos.add("path2", 1);

    po::variables_map vm;
    po::store(po::command_line_parser(argc, argv).
              options(opt).positional(pos).run(), vm);
    po::notify(vm);

    if (vm.count("help") > 0) {
        std::cerr << opt << std::endl;
        return 0;
    }

    // Read booleans.
    opts.hips_rotate = (vm.count("no-rotate") < 1);

    std::cerr << "Reading image 1 (" << path1 << ")" << std::endl;
    GrayImage   g1image_full;
    DepthImage  d1image_full;
    readRGBD(g1image_full, d1image_full, path1.c_str());

    std::cerr << "Reading image 2 (" << path2 << ")" << std::endl;
    GrayImage   g2image_full;
    DepthImage  d2image_full;
    readRGBD(g2image_full, d2image_full, path2.c_str());

    GrayImage   g1image(256, 512, 4);
    g1image = g1image_full(blitz::Range(0, 255), blitz::Range(80, 80 + 511), blitz::Range::all());

    GrayImage   g2image(256, 512, 4);
    g2image = g2image_full(blitz::Range(0, 255), blitz::Range(80, 80 + 511), blitz::Range::all());

    DepthImage  d1image(256, 512, 1);
    d1image = d1image_full(blitz::Range(0, 255), blitz::Range(80, 80 + 511), blitz::Range::all());

    DepthImage  d2image(256, 512, 1);
    d2image = d2image_full(blitz::Range(0, 255), blitz::Range(80, 80 + 511), blitz::Range::all());

    // Create structure for stage 1 input.
    stage1input const input = {g1image, g2image, d1image, d2image, opts};

    try {
        // Prepare list for all OpenCL devices on all platforms.
        std::vector<cl::Device> devices;

        std::vector<cl::Platform> platforms;
        cl::Platform::get(&platforms);

        std::cerr << "Found " << platforms.size() << " OpenCL platforms" << std::endl;

        for (size_t ip = 0; ip < platforms.size(); ip++) {
            cl::Platform &pf = platforms.at(ip);

            std::cerr
                << pf.getInfo<CL_PLATFORM_NAME   >() << " ("
                << pf.getInfo<CL_PLATFORM_VENDOR >() << ", "
                << pf.getInfo<CL_PLATFORM_VERSION>() << ")"
                << std::endl;

            std::vector<cl::Device> newDevices;

            try {
                pf.getDevices(CL_DEVICE_TYPE_ALL, &newDevices);
                std::cerr << "  Found " << newDevices.size() << " OpenCL devices" << std::endl;
            } catch (cl::Error & err) {
                std::cerr << err.what() << " (code " << err.err() << ")" << std::endl;
            }

            for (size_t id = 0; id < newDevices.size(); id++) {
                cl::Device &dev = newDevices.at(id);

                std::cerr << "    " << dev.getInfo<CL_DEVICE_NAME>() << std::endl;

                std::cerr << "      Compute units:  " << std::setw(8) <<
                        dev.getInfo<CL_DEVICE_MAX_COMPUTE_UNITS>() << std::endl;

                std::cerr << "      Global memory:  " << std::setw(8) <<
                        (dev.getInfo<CL_DEVICE_GLOBAL_MEM_SIZE>() / MiB) <<
                        " MiB" << std::endl;

                // Add to list of all devices.
                devices.push_back(dev);
            }
        }

        std::cerr << std::endl << std::endl;

        for (size_t id = 0; id < devices.size(); id++) {
            cl::Device &dev = devices.at(id);

            std::cerr << "Running pipeline for \"" << dev.getInfo<CL_DEVICE_NAME>() << "\"" << std::endl;

            try {
                testPipeline(dev, input);
            } catch (cl::Error & err) {
                std::cerr << err.what() << " (code " << err.err() << ")" << std::endl;
            }
        }
    } catch (cl::Error & err) {
        std::cerr << err.what() << " (code " << err.err() << ")" << std::endl;
    }
    return 0;
}
