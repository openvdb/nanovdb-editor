// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

/*!
    \file   nanovdb_editor/app/main.cpp

    \author Petra Hapalova, Andrew Reidmeyer

    \brief
*/

#include "Log.h"
#include "Node2Convert.h"

#include <nanovdb_editor/putil/Editor.h>

#include <nanovdb/io/IO.h>
#include <nanovdb/tools/CreatePrimitives.h>

#include <argparse/argparse.hpp>

#include <stdio.h>
#include <stdarg.h>
#include <math.h>

#define CONVERT_NODE2 1

struct NanoVDBEditorArgs : public argparse::Args
{
    std::string& input_file = kwarg("i,input", "Input NanoVDB file path").set_default("./data/dragon.nvdb");
    bool& convert_node2 = flag("c,convert", "Convert to Node2 format").set_default(false);
    std::string& convert_node2_output_file = kwarg("o,output", "Convert to Node2 output file path").set_default("");
    bool& headless = flag("headless", "Run in headless mode").set_default(false);
    bool& streaming = flag("s,stream", "Run in streaming mode").set_default(false);
    bool& stream_to_file = flag("stream-to-file", "Stream to file").set_default(false);
    std::string& ip_address = kwarg("ip,address", "IP address for streaming").set_default("127.0.0.1");
    int& port = kwarg("p,port", "Port for streaming").set_default(8080);
};

int main(int argc, char* argv[])
{
    auto args = argparse::parse<NanoVDBEditorArgs>(argc, argv);
    // args.print();

    printf("NanoVDB Editor starting...\n");
    printf("Input file: '%s'\n", args.input_file.c_str());
    if (!args.convert_node2_output_file.empty())
    {
        printf("Output file: '%s'\n", args.convert_node2_output_file.c_str());
    }

#if CONVERT_NODE2
#    ifndef _DEBUG
    printf("Convert to Node2: %s\n", args.convert_node2 ? "true" : "false");
    if (args.convert_node2)
    {
        std::string input_path = args.input_file;
        std::string output_path = args.convert_node2_output_file.empty() ?
                                      input_path.substr(0, input_path.find_last_of('.')) + "_node2.nvdb" :
                                      args.convert_node2_output_file;

        pnanovdb_editor::node2_convert(input_path.c_str(), output_path.c_str());
        // pnanovdb_editor::node2_sphere(input_path.c_str());
        printf("Converted '%s' to '%s'\n", input_path.c_str(), output_path.c_str());
    }
#    endif
#endif

    printf("Headless mode: %s\n", args.headless ? "true" : "false");
    printf("Streaming mode: %s\n", args.streaming ? "true" : "false");
    printf("Stream to file: %s\n", args.stream_to_file ? "true" : "false");
    printf("IP address: %s\n", args.ip_address.c_str());
    printf("Port: %d\n", args.port);

    pnanovdb_compiler_t compiler = {};
    pnanovdb_compiler_load(&compiler);

    pnanovdb_compute_t compute = {};
    pnanovdb_compute_load(&compute, &compiler);

    pnanovdb_compute_device_desc_t device_desc = {};
    device_desc.log_print = pnanovdb_compute_log_print;

    pnanovdb_compute_device_manager_t* device_manager = compute.device_interface.create_device_manager(PNANOVDB_FALSE);
    pnanovdb_compute_device_t* device = compute.device_interface.create_device(device_manager, &device_desc);

    const char* file = args.input_file.c_str();

    pnanovdb_editor_t editor = {};
    pnanovdb_editor_load(&editor, &compute, &compiler);

    pnanovdb_compute_array_t* data_in = compute.load_nanovdb(file);
    editor.add_nanovdb(&editor, data_in);

    pnanovdb_editor_config_t config = {};
    config.ip_address = args.ip_address.c_str();
    config.port = args.port;
    config.headless = args.headless ? PNANOVDB_TRUE : PNANOVDB_FALSE;
    config.streaming = args.streaming ? PNANOVDB_TRUE : PNANOVDB_FALSE;
    config.stream_to_file = args.stream_to_file ? PNANOVDB_TRUE : PNANOVDB_FALSE;
    editor.show(&editor, device, &config);

    pnanovdb_editor_free(&editor);

    compute.device_interface.destroy_device(device_manager, device);
    compute.device_interface.destroy_device_manager(device_manager);

    pnanovdb_compute_free(&compute);
    pnanovdb_compiler_free(&compiler);

    return 0;
}
