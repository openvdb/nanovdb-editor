# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import argparse
import sys
import nanovdb_editor as nve
from time import sleep

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="NanoVDB Editor")
    parser.add_argument("--ip", default="192.168.0.6", help="IP address to bind to (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8080, help="Port to bind to (default: 8080)")
    parser.add_argument("--headless", action="store_true", help="Run in headless mode")
    parser.add_argument("--stream", action="store_true", help="Enable streaming mode")
    parser.add_argument("--device", type=int, default=0, help="Vulkan device index to use (default: 0)")

    args = parser.parse_args()

    try:
        editor, compute, compiler = nve.create_default(device_id=args.device)
    except Exception as e:
        print(f"Error initializing editor: {e}")
        sys.exit(1)

    config = nve.EditorConfig()
    config.ip_address = args.ip.encode("utf-8")
    config.port = args.port
    config.headless = 1 if args.headless else 0
    config.streaming = 1 if args.stream else 0

    try:
        if args.headless:
            editor.start(config)
            print("Editor running at {}:{}.. Ctrl+C to exit".format(args.ip, args.port))
            editor.wait_for_interrupt()
        else:
            editor.show(config)
    except Exception as e:
        print(f"Error starting editor: {e}")
    finally:
        print("Shutting down editor...")
        if args.headless:
            editor.stop()
        editor = None
        compute = None
        compiler = None
