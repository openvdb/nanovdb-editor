# Copyright Contributors to the OpenVDB Project
# SPDX-License-Identifier: Apache-2.0

import argparse
import sys
import nanovdb_editor as nve

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="NanoVDB Editor")
    parser.add_argument("--ip", default="192.168.0.6", help="IP address to bind to (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8080, help="Port to bind to (default: 8080)")
    parser.add_argument("--headless", action="store_true", help="Run in headless mode")
    parser.add_argument("--stream", action="store_true", help="Enable streaming mode")
    parser.add_argument("--device", type=int, default=0, help="Vulkan device index to use (default: 0)")

    args = parser.parse_args()

    try:
        session = nve.create_default(device_id=args.device)
    except Exception as e:
        print(f"Error initializing editor: {e}")
        sys.exit(1)

    try:
        if args.headless:
            session.start(ip=args.ip, port=args.port, headless=True, streaming=args.stream)
            print("Editor running at {}:{}.. Ctrl+C to exit".format(args.ip, args.port))
            session.wait_for_interrupt()
        else:
            session.show(ip=args.ip, port=args.port, streaming=args.stream)
    except Exception as e:
        print(f"Error starting editor: {e}")
    finally:
        print("Shutting down editor...")
        session.close()
