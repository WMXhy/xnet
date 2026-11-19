import argparse
from pathlib import Path
import re
import select
import shutil
import socket
import subprocess
import tempfile


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(*args):
    command = [str(arg) for arg in args]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    require(result.returncode == 0, f"{command!r}\n{result.stdout}")
    return result.stdout


def test_server(executable, chunks, expected, wait_after_first=False):
    with subprocess.Popen(
        [str(executable)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True,
    ) as server:
        try:
            require(select.select([server.stdout], [], [], 5)[0], "示例没有发布监听端口")
            line = server.stdout.readline().strip()
            address = re.fullmatch(r"Listening on 127\.0\.0\.1:(\d+)", line)
            require(address is not None, f"示例启动失败：{line}")
            with socket.create_connection(("127.0.0.1", int(address[1])), timeout=4) as client:
                for index, chunk in enumerate(chunks):
                    client.sendall(chunk)
                    if index == 0 and wait_after_first:
                        require(not select.select([client], [], [], 0.1)[0], "半行被提前处理")
                client.shutdown(socket.SHUT_WR)
                received = bytearray()
                while True:
                    chunk = client.recv(4096)
                    if not chunk:
                        break
                    received.extend(chunk)
                    require(len(received) <= len(expected), "示例返回了额外数据")
                require(received == expected, f"示例返回错误：{received!r}")
            output, _ = server.communicate(input="\n", timeout=5)
            require(server.returncode == 0, f"示例停机失败：{output}")
        finally:
            if server.poll() is None:
                server.kill()
                server.communicate(timeout=5)


def verify_installed_targets(build, source, package_build, old_prefix, prefix):
    path_list = (build / "xnet_paths_Release.txt").read_text().splitlines()
    require(len(path_list) == 2, f"无效的导入目标路径：{path_list}")
    library = Path(path_list[0]).resolve()
    headers = [Path(entry).resolve() for entry in path_list[1].split(";") if entry]
    require(library.is_file() and library.name == "libxnet.a" and prefix in library.parents,
            f"库来自安装前缀以外：{library}")
    require(headers and all(prefix == header or prefix in header.parents for header in headers),
            f"头文件路径来自安装前缀以外：{headers}")
    for name in ("connection.hpp", "network_buffer.hpp", "tcp_server.hpp"):
        require(any((entry / "xnet" / name).is_file() for entry in headers), f"缺少公开头 {name}")
    for config in prefix.rglob("*.cmake"):
        text = config.read_text()
        require(all(str(path) not in text for path in (source, package_build, old_prefix)),
                f"安装包保留了源码、构建或原安装前缀路径：{config}")
    return library, headers


def check_package(args, scratch):
    old_prefix = scratch / "initial-prefix"
    prefix = scratch / "relocated prefix"
    consumer = scratch / "external"
    build = scratch / "external-build"

    install = [args.cmake, "--install", args.build, "--prefix", old_prefix]
    if args.config:
        install.extend(("--config", args.config))
    run(*install)
    old_prefix.rename(prefix)

    shutil.copytree(Path(args.source) / "examples" / "installed", consumer)
    # 在外部项目的临时副本中记录导入目标路径，保持发布示例本身简洁。
    with (consumer / "CMakeLists.txt").open("a") as cmake:
        cmake.write(
            '\nfile(GENERATE OUTPUT "xnet_paths_$<CONFIG>.txt"\n'
            '    CONTENT "$<TARGET_FILE:xnet::xnet>\\n'
            '$<TARGET_PROPERTY:xnet::xnet,INTERFACE_INCLUDE_DIRECTORIES>\\n")\n'
        )
    run(args.cmake, "-S", consumer, "-B", build, "-G", args.generator,
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_PREFIX_PATH={prefix}",
        "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF",
        "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF")
    library, headers = verify_installed_targets(
        build, Path(args.source).resolve(), Path(args.build).resolve(), old_prefix, prefix)
    log = run(args.cmake, "--build", build, "--config", "Release", "--parallel", "2", "--verbose")
    for path in [library, *headers]:
        require(str(path) in log or str(path).replace(" ", "\\ ") in log,
                f"示例构建命令未使用搬迁后的路径：{path}")
    binaries = build / "Release" if (build / "Release" / "xnet_echo").is_file() else build
    test_server(binaries / "xnet_echo", [b"xnet\x00", b"\xff tcp\n"], b"xnet\x00\xff tcp\n")
    test_server(binaries / "xnet_line_echo", [b"frag", b"ment\nsecond\nincomplete"],
                b"line: fragment\nline: second\n", wait_after_first=True)
    print("搬迁后的 CMake 包已通过纯安装树消费验证，两个回环示例均通过。")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for name in ("cmake", "source", "build", "generator"):
        parser.add_argument(f"--{name}", required=True)
    parser.add_argument("--config", default="")
    with tempfile.TemporaryDirectory(prefix="xnet-package-") as scratch:
        check_package(parser.parse_args(), Path(scratch).resolve())
