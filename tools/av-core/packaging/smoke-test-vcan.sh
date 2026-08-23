#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "SKIP : ce smoke test nécessite Linux."
    exit 77
fi
if [[ "${EUID}" -ne 0 ]]; then
    echo "SKIP : lancez ce smoke test avec sudo."
    exit 77
fi
if /usr/sbin/ip link show dev can0 >/dev/null 2>&1; then
    echo "REFUS : can0 existe déjà; aucun test vcan ne sera lancé." >&2
    exit 2
fi

modprobe vcan
/usr/sbin/ip link add dev can0 type vcan
trap '/usr/sbin/ip link delete dev can0 2>/dev/null || true' EXIT
/usr/sbin/ip link set dev can0 up

python_path=/opt/fsotelemetry/av-core/current/.venv/bin/python
if [[ ! -x "$python_path" ]]; then
    echo "AV CORE n'est pas installé sous /opt/fsotelemetry/av-core/current." >&2
    exit 3
fi

runuser -u fsotelemetry -- "$python_path" - <<'PY'
import can

bus = can.Bus(interface="socketcan", channel="can0", receive_own_messages=True)
try:
    message = can.Message(arbitration_id=0x184, data=bytes((1, 0x81, 2, 0, 0, 0, 0, 0)), is_extended_id=False)
    bus.send(message, timeout=0.1)
    received = bus.recv(timeout=1.0)
    if received is None or received.arbitration_id != 0x184 or bytes(received.data) != bytes(message.data):
        raise SystemExit("échec de la boucle SocketCAN vcan")
finally:
    bus.shutdown()
PY

echo "Smoke test SocketCAN vcan réussi."
