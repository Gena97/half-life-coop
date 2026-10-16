import argparse
import json
import re
import socket
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description="Локальное управление проверочным HLDS")
    parser.add_argument("marker", type=Path, help="Путь к hlcoop_tc/.hlcoop-transition-check.json")
    parser.add_argument("command", nargs="+", help="Команда сервера, например status или hctcheck_stop")
    args = parser.parse_args()

    # Пароль и порт берём только из метки отдельного проверочного мода
    marker = json.loads(args.marker.read_text(encoding="utf-8-sig"))
    if marker.get("project") != "half-life_coop/transition-check" or marker.get("format") != 1:
        raise ValueError("Метка не принадлежит проверочному окружению")
    password = marker["rconPassword"]
    port = int(marker["port"])
    if not re.fullmatch(r"[0-9a-f]{32}", password) or not 1024 <= port <= 65535:
        raise ValueError("Некорректные параметры RCON в метке")
    command = " ".join(args.command)
    if any(char in command for char in ("\r", "\n", "\0")):
        raise ValueError("Команда должна занимать одну строку")

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
        client.settimeout(2)
        client.connect(("127.0.0.1", port))
        client.send(b"\xff\xff\xff\xffchallenge rcon\n")
        response = client.recv(65535)
        challenge = re.search(rb"challenge rcon (-?\d+)", response)
        if not challenge:
            raise ValueError("Сервер не вернул RCON challenge")
        request = f'rcon {challenge[1].decode("ascii")} "{password}" {command}\n'
        client.send(b"\xff\xff\xff\xff" + request.encode("ascii"))
        try:
            response = client.recv(65535)
        except (socket.timeout, ConnectionResetError):
            if command.strip().lower() == "quit":
                return
            raise
        if response.startswith(b"\xff\xff\xff\xffl"):
            response = response[5:]
        print(response.rstrip(b"\0\r\n").decode("utf-8", errors="replace"))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError) as error:
        print(f"RCON: {error}", file=sys.stderr)
        sys.exit(1)
