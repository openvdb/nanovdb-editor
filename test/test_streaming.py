# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import argparse
from nanovdb_editor import Compiler, Compute, Editor, pnanovdb_EditorConfig

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='NanoVDB Editor Server')
    parser.add_argument('--ip', default='127.0.0.1',
                       help='IP address to bind to (default: 127.0.0.1)')
    parser.add_argument('--port', type=int, default=8080,
                       help='Port to bind to (default: 8080)')

    args = parser.parse_args()

    compiler = Compiler()
    compiler.create_instance()

    compute = Compute(compiler)

    compute.device_interface().create_device_manager()
    compute.device_interface().create_device()

    editor = Editor(compute, compiler)

    config = pnanovdb_EditorConfig()
    config.ip_address = args.ip.encode('utf-8')
    config.port = args.port
    config.headless = 1
    config.streaming = 1
    editor.show(config)
