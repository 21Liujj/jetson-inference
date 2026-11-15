#!/usr/bin/env python3
"""M0: read-only evidence collector. Missing tools remain explicit failures."""
import argparse
import datetime as dt
import hashlib
import json
import pathlib
import platform
import shutil
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--repo", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--device", required=True, help="Explicit capture node, preferably /dev/v4l/by-id/…-index0")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    commands = {
        "uname": ["uname", "-a"],
        "os": ["cat", "/etc/os-release"],
        "l4t": ["cat", "/etc/nv_tegra_release"],
        "packages": ["dpkg-query", "-W", "-f=${db:Status-Abbrev} ${binary:Package} ${Version}\n",
                     "nvidia-l4t-*", "libnvinfer*", "cuda-cudart-*", "ros-humble-ros-base", "ros-humble-vision-msgs"],
        "memory": ["free", "-h"],
        "disk": ["df", "-h", str(args.repo)],
        "usb": ["lsusb"],
        "usb_topology": ["lsusb", "-t"],
        "video_devices": ["v4l2-ctl", "--list-devices"],
        "camera_caps": ["v4l2-ctl", "-d", args.device, "--all"],
        "camera_formats": ["v4l2-ctl", "-d", args.device, "--list-formats-ext"],
        "camera_owners": ["fuser", args.device],
        "uvc_module": ["modinfo", "uvcvideo"],
        "compiler": ["c++", "--version"],
        "cmake": ["cmake", "--version"],
        "cuda": ["/usr/local/cuda/bin/nvcc", "--version"],
        "tensorrt": ["/usr/src/tensorrt/bin/trtexec", "--help"],
        "nsys": ["nsys", "--version"],
        "ros2": ["bash", "-c", "source /opt/ros/humble/setup.bash && ros2 --help && ros2 pkg prefix vision_msgs"],
        "git_head": ["git", "-C", str(args.repo), "rev-parse", "HEAD"],
        "git_status": ["git", "-C", str(args.repo), "status", "--short", "--branch"],
        "git_remotes": ["git", "-C", str(args.repo), "remote", "-v"],
        "git_submodules": ["git", "-C", str(args.repo), "submodule", "status", "--recursive"],
        "power_mode": ["/usr/sbin/nvpmodel", "-q"],
        "service_state": ["systemctl", "show", "edgevision.service", "-p", "ActiveState", "-p", "SubState"],
    }
    result = {"schema_version": 1, "simulated": False, "scope": "environment_only",
              "wall_time": dt.datetime.now(dt.timezone.utc).isoformat(), "platform": platform.platform(),
              "device": args.device, "commands": {},
              "tools_on_path": {x: shutil.which(x) for x in ("nvcc", "trtexec", "nsys", "ros2", "colcon", "v4l2-ctl")}}
    for name, argv in commands.items():
        start = time.monotonic_ns()
        try:
            p = subprocess.run(argv, capture_output=True, text=True, timeout=25, errors="replace")
            rc, stdout, stderr = p.returncode, p.stdout, p.stderr
        except (OSError, subprocess.TimeoutExpired) as exc:
            rc, stdout, stderr = None, "", str(exc)
        raw = json.dumps({"argv": argv, "exit_code": rc, "stdout": stdout, "stderr": stderr,
                          "elapsed_ns": time.monotonic_ns() - start}, ensure_ascii=False, indent=2)
        target = args.out / (name + ".json")
        target.write_text(raw + "\n", encoding="utf-8")
        result["commands"][name] = {"file": target.name, "exit_code": rc,
                                    "sha256": hashlib.sha256(target.read_bytes()).hexdigest()}
    (args.out / "env.json").write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps({"out": str(args.out), "simulated": False,
                      "commands": len(commands), "nonzero": [k for k, v in result["commands"].items() if v["exit_code"] != 0]}))


if __name__ == "__main__":
    main()
