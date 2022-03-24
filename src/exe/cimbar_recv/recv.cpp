/* This code is subject to the terms of the Mozilla Public License, v.2.0. http://mozilla.org/MPL/2.0/. */
#include "cimb_translator/Config.h"
#include "compression/zstd_decompressor.h"
#include "encoder/Decoder.h"
#include "extractor/Extractor.h"
#include "fountain/fountain_decoder_sink.h"
#include "gui/window_glfw.h"

#include "cxxopts/cxxopts.hpp"
#include "serialize/str.h"
#include "util/File.h"
#include <opencv2/videoio.hpp>
#include <GLFW/glfw3.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
using std::string;
using std::vector;

namespace {

	template <typename TP>
	TP wait_for_frame_time(unsigned delay, const TP& start)
	{
		unsigned millis = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start).count();
		if (delay > millis)
			std::this_thread::sleep_for(std::chrono::milliseconds(delay-millis));
		return std::chrono::high_resolution_clock::now();
	}


	std::pair<int, int> window_size()
	{
		GLFWmonitor* mon = glfwGetPrimaryMonitor();
		const GLFWvidmode* mode = glfwGetVideoMode(mon);
		return {mode->width, mode->height};
	}
}


int main(int argc, char** argv)
{
	cxxopts::Options options("cimbar video decoder", "Use the camera to decode data!");

	unsigned colorBits = cimbar::Config::color_bits();
	unsigned ecc = cimbar::Config::ecc_bytes();
	unsigned defaultFps = 15;
	options.add_options()
		("i,in", "Video source.", cxxopts::value<string>())
		("o,out", "Output directory (decoding).", cxxopts::value<string>())
	    ("c,colorbits", "Color bits. [0-3]", cxxopts::value<int>()->default_value(turbo::str::str(colorBits)))
		("e,ecc", "ECC level", cxxopts::value<unsigned>()->default_value(turbo::str::str(ecc)))
		("f,fps", "Target FPS", cxxopts::value<unsigned>()->default_value(turbo::str::str(defaultFps)))
	    ("h,help", "Print usage")
	;
	options.show_positional_help();
	options.parse_positional({"in", "out"});
	options.positional_help("<in> <out>");

	auto result = options.parse(argc, argv);
	if (result.count("help") or !result.count("in") or !result.count("out"))
	{
	  std::cout << options.help() << std::endl;
	  return 0;
	}

	string source = result["in"].as<string>();
	string outpath = result["out"].as<string>();

	colorBits = std::min(3, result["colorbits"].as<int>());
	ecc = result["ecc"].as<unsigned>();
	unsigned fps = result["fps"].as<unsigned>();
	if (fps == 0)
		fps = defaultFps;
	unsigned delay = 1000 / fps;

	cv::VideoCapture vc(source.c_str());
	if (!vc.isOpened())
	{
		std::cerr << "failed to open video device :(" << std::endl;
		return 70;
	}

	vc.set(cv::CAP_PROP_FRAME_WIDTH, 1920);
	vc.set(cv::CAP_PROP_FRAME_HEIGHT, 1080);

	//auto [width, height] = window_size();
	int width = 1920;
	int height = 1080;
	std::cout << "got dimensions " << width << "," << height << std::endl;

	cimbar::window_glfw window(width, height, "cimbar_recv");
	if (!window.is_good())
	{
		std::cerr << "failed to create window :(" << std::endl;
		return 70;
	}

	Extractor ext;
	Decoder dec;

	unsigned chunkSize = cimbar::Config::fountain_chunk_size(ecc);
	fountain_decoder_sink<cimbar::zstd_decompressor<std::ofstream>> sink(outpath, chunkSize);

	cv::Mat mat;

	std::chrono::time_point start = std::chrono::high_resolution_clock::now();
	while (true)
	{
		// delay, then try to read frame
		start = wait_for_frame_time(delay, start);
		if (window.should_close())
			break;

		if (!vc.read(mat))
		{
			std::cerr << "failed to read from cam" << std::endl;
			continue;
		}
		cv::UMat img = mat.getUMat(cv::ACCESS_RW);

		cv::cvtColor(mat, mat, cv::COLOR_BGR2RGB);

		// draw some stats on mat?
		window.show(mat, 0);

		// extract
		bool shouldPreprocess = true;
		int res = ext.extract(img, img);
		if (!res)
		{
			std::cerr << "no extract " << mat.cols << "," << mat.rows << std::endl;
			continue;
		}
		else if (res == Extractor::NEEDS_SHARPEN)
			shouldPreprocess = true;

		cv::imwrite("/tmp/foo-base.png", mat);
		cv::imwrite("/tmp/foo-ex.png", img);

		// decode
		int bytes = dec.decode_fountain(img, sink, shouldPreprocess);
		std::cerr << "got some bytes " << bytes << std::endl;

	}

	return 0;
}
