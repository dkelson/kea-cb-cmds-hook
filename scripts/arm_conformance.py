#!/usr/bin/env python3
"""ARM conformance harness for kea-cb-cmds-hook (libdhcp_cb_cmds.so).

Spins isolated kea-dhcp4 + kea-dhcp6 instances wired to a PostgreSQL or MySQL
config backend (config-control + database hook + libdhcp_cb_cmds.so), drives
every remote-* command in its ARM 3.0 *canonical* request shape, and prints a
PASS/DEVIATION table comparing the hook's behaviour to the documented Kea ARM
contract. All config-backend rows it creates are torn down.

Reference: https://kea.readthedocs.io/en/kea-3.0.0/arm/hooks.html
           (cb_cmds: Configuration Backend Commands)

Requirements on the host running this:
  - kea-dhcp4 / kea-dhcp6 binaries
  - database hook + libdhcp_cb_cmds.so in the hooks dir
  - a Kea config-backend DB initialised (`kea-admin db-init pgsql|mysql`)

Configuration via environment (sane defaults shown):
  KEA_BIN4=kea-dhcp4            KEA_BIN6=kea-dhcp6
  KEA_HOOKS_DIR=/usr/lib64/kea/hooks
  CB_DB_TYPE=postgresql         (postgresql or mysql)
  CB_PG_HOST=127.0.0.1  CB_PG_PORT=5432  CB_PG_NAME=kea  CB_PG_USER=kea
  CB_PG_PASSWORD=...           (required for PostgreSQL)
  CB_MYSQL_HOST=127.0.0.1  CB_MYSQL_PORT=3306  CB_MYSQL_NAME=kea
  CB_MYSQL_USER=kea  CB_MYSQL_PASSWORD=...     (required for MySQL)
  CB_PORT4=18010  CB_PORT6=18011
  KEA_CB_CMDS_ARM_CONFORMANCE_RUN_DIR=/path/to/results  (optional; preserved)

Run:   CB_PG_PASSWORD=... python3 scripts/arm_conformance.py
Exit:  0 if fully conformant, 1 if any DEVIATION is found.

Each check states the ARM-canonical request; a DEVIATION means the hook rejected
or mishandled a request that the ARM contract accepts.
"""
import base64
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.request

HOOKS = os.environ.get("KEA_HOOKS_DIR", "/usr/lib64/kea/hooks")
BACKENDS = {
    "postgresql": {
        "aliases": ("postgresql", "postgres", "pgsql"),
        "env_prefixes": ("CB_PG",),
        "default_port": 5432,
        "hook": "pgsql",
        "label": "PostgreSQL",
    },
    "mysql": {
        "aliases": ("mysql", "mariadb"),
        "env_prefixes": ("CB_MYSQL", "CB_MY"),
        "default_port": 3306,
        "hook": "mysql",
        "label": "MySQL",
    },
}


def normalize_backend(name):
    for backend, config in BACKENDS.items():
        if name.lower() in config["aliases"]:
            return backend
    raise SystemExit("unsupported CB_DB_TYPE %r; expected postgresql or mysql" % name)


def env_value(prefixes, key, default=""):
    for prefix in prefixes:
        value = os.environ.get("%s_%s" % (prefix, key))
        if value:
            return value
    return default


def select_backend():
    requested = os.environ.get("CB_DB_TYPE")
    if requested:
        return normalize_backend(requested)
    if env_value(BACKENDS["mysql"]["env_prefixes"], "PASSWORD") and not os.environ.get("CB_PG_PASSWORD"):
        return "mysql"
    return "postgresql"


BACKEND = select_backend()
BACKEND_CONFIG = BACKENDS[BACKEND]
DB = {"type": BACKEND,
      "host": env_value(BACKEND_CONFIG["env_prefixes"], "HOST", "127.0.0.1"),
      "port": int(env_value(BACKEND_CONFIG["env_prefixes"], "PORT",
                            str(BACKEND_CONFIG["default_port"]))),
      "name": env_value(BACKEND_CONFIG["env_prefixes"], "NAME", "kea"),
      "user": env_value(BACKEND_CONFIG["env_prefixes"], "USER", "kea"),
      "password": env_value(BACKEND_CONFIG["env_prefixes"], "PASSWORD", "")}
REMOTE = {"type": BACKEND}                # the "remote" selector in every command
AUTH = base64.b64encode(b"c:c").decode()
results = []                              # (family, label, ok, detail)
RUN_DIR = None
PRESERVE_RUN_DIR = False


def daemon_conf(family, port):
    lease_file = os.path.join(RUN_DIR, "armconf-leases%d.csv" % family)
    top = "Dhcp%d" % family
    return {top: {
        "interfaces-config": {"interfaces": []},
        "lease-database": {"type": "memfile", "persist": False,
                           "name": lease_file},
        "server-tag": "conf%d" % family,
        "config-control": {"config-databases": [dict(DB)], "config-fetch-wait-time": 3},
        "hooks-libraries": [{"library": "%s/libdhcp_%s.so" % (HOOKS, BACKEND_CONFIG["hook"])},
                            {"library": HOOKS + "/libdhcp_cb_cmds.so"}],
        "control-sockets": [{"socket-type": "http", "socket-address": "127.0.0.1",
                             "socket-port": port, "authentication": {
                                 "type": "basic", "realm": "c",
                                 "clients": [{"user": "c", "password": "c"}]}}],
        "subnet%d" % family: [],
        "loggers": [{"name": "kea-dhcp%d" % family,
                     "output_options": [{"output": "stdout"}], "severity": "ERROR"}]}}


class Daemon:
    def __init__(self, family, port):
        self.family, self.port = family, port
        self.bin = os.environ.get("KEA_BIN%d" % family, "kea-dhcp%d" % family)

    def __enter__(self):
        fd, self.path = tempfile.mkstemp(suffix=".conf", prefix="armconf%d-" % self.family,
                                         dir=RUN_DIR)
        os.write(fd, json.dumps(daemon_conf(self.family, self.port)).encode())
        os.close(fd)
        self.log_path = os.path.join(RUN_DIR, "armconf-out%d.log" % self.family)
        self.log = open(self.log_path, "w")
        env = os.environ.copy()
        env.update({
            "KEA_DHCP_DATA_DIR": RUN_DIR,
            "KEA_LOCKFILE_DIR": RUN_DIR,
            "KEA_LOG_FILE_DIR": RUN_DIR,
            "KEA_PIDFILE_DIR": RUN_DIR,
        })
        self.p = subprocess.Popen([self.bin, "-X", "-c", self.path],
                                  stdout=self.log, stderr=subprocess.STDOUT, env=env)
        for _ in range(40):                      # wait for the control socket
            time.sleep(0.25)
            try:
                self.call("config-get", {})
                return self
            except Exception:
                if self.p.poll() is not None:
                    self.log.flush()
                    with open(self.log_path) as log:
                        detail = log.read()[-2000:]
                    raise RuntimeError("kea-dhcp%d exited:\n%s"
                                       % (self.family, detail))
        raise RuntimeError("kea-dhcp%d control socket never came up" % self.family)

    def __exit__(self, *a):
        self.p.terminate()
        try:
            self.p.wait(timeout=5)
        except Exception:
            self.p.kill()
        self.log.close()
        if not PRESERVE_RUN_DIR:
            os.unlink(self.path)

    def call(self, cmd, args):
        body = json.dumps({"command": cmd, "arguments": args}).encode()
        req = urllib.request.Request("http://127.0.0.1:%d/" % self.port, data=body,
                                     headers={"Content-Type": "application/json",
                                              "Authorization": "Basic " + AUTH})
        r = json.loads(urllib.request.urlopen(req, timeout=15).read())[0]
        return r.get("result"), r.get("text", ""), r.get("arguments", {})


def check(d, label, cmd, args, want=0):
    """Run one ARM-canonical command; PASS iff result == want (0 = success)."""
    try:
        res, txt, _ = d.call(cmd, args)
    except Exception as e:
        res, txt = "EXC", str(e)[:80]
    ok = (res == want)
    results.append((d.family, label, ok, "result=%s %r" % (res, txt[:70])))
    print("  %-4s %-44s result=%-4s %s" % ("PASS" if ok else "DEV!", label, res, txt[:60]))
    return ok


def run_family(family, port):
    pool = "192.0.2.10-192.0.2.100" if family == 4 else "2001:db8:1::10-2001:db8:1::100"
    prefix = "192.0.2.0/24" if family == 4 else "2001:db8:1::/64"
    sid = 71700000 + family
    net = "armnet%d" % family
    optname = "domain-name-servers" if family == 4 else "dns-servers"
    optdata = "192.0.2.1" if family == 4 else "2001:db8:1::1"
    optcode = 6 if family == 4 else 23
    space = "dhcp%d" % family
    st = ["conf%d" % family]
    print("\n=== DHCPv%d ===" % family)
    with Daemon(family, port) as d:
        c = lambda *a, **k: check(d, *a, **k)
        # --- server: set takes the `servers` list; get/del should too (ARM) ---
        c("server-set", "remote-server%d-set" % family,
          {"remote": REMOTE, "servers": [{"server-tag": "conf%d" % family, "description": "x"}]})
        c("server-get  [ARM servers-list form]", "remote-server%d-get" % family,
          {"remote": REMOTE, "servers": [{"server-tag": "conf%d" % family}]})
        c("server-get-all", "remote-server%d-get-all" % family, {"remote": REMOTE})
        # --- subnet (our hot path: explicit id + shared-network-name:null + relay) ---
        subnet = {"id": sid, "subnet": prefix, "shared-network-name": None,
                  "relay": {"ip-addresses": [optdata]}, "pools": [{"pool": pool}]}
        c("subnet-set [id+relay]", "remote-subnet%d-set" % family,
          {"remote": REMOTE, "server-tags": st, "subnets": [subnet]})
        c("subnet-get-by-id", "remote-subnet%d-get-by-id" % family,
          {"remote": REMOTE, "subnets": [{"id": sid}]})
        c("subnet-get-by-prefix", "remote-subnet%d-get-by-prefix" % family,
          {"remote": REMOTE, "subnets": [{"subnet": prefix}]})
        c("subnet-list", "remote-subnet%d-list" % family, {"remote": REMOTE, "server-tags": st})
        # --- global parameter ---
        c("global-parameter-set", "remote-global-parameter%d-set" % family,
          {"remote": REMOTE, "server-tags": st, "parameters": {"renew-timer": 1000}})
        c("global-parameter-get", "remote-global-parameter%d-get" % family,
          {"remote": REMOTE, "server-tags": st, "parameters": ["renew-timer"]})
        c("global-parameter-get-all", "remote-global-parameter%d-get-all" % family,
          {"remote": REMOTE, "server-tags": st})
        # --- shared network ---
        c("network-set", "remote-network%d-set" % family,
          {"remote": REMOTE, "server-tags": st, "shared-networks": [{"name": net}]})
        c("network-get", "remote-network%d-get" % family,
          {"remote": REMOTE, "shared-networks": [{"name": net}]})
        c("network-list", "remote-network%d-list" % family, {"remote": REMOTE, "server-tags": st})
        # --- option definition: ARM makes array/record-types/encapsulate OPTIONAL ---
        c("option-def-set [ARM: no array/record-types/encapsulate]",
          "remote-option-def%d-set" % family,
          {"remote": REMOTE, "server-tags": st,
           "option-defs": [{"name": "my-opt", "code": 222, "type": "uint32", "space": space}]})
        # create one with all fields so the get below has a target
        d.call("remote-option-def%d-set" % family,
               {"remote": REMOTE, "server-tags": st,
                "option-defs": [{"name": "my-opt", "code": 222, "type": "uint32", "space": space,
                                 "array": False, "record-types": "", "encapsulate": ""}]})
        c("option-def-get", "remote-option-def%d-get" % family,
          {"remote": REMOTE, "server-tags": st, "option-defs": [{"code": 222, "space": space}]})
        c("option-def-get-all", "remote-option-def%d-get-all" % family,
          {"remote": REMOTE, "server-tags": st})
        # --- options at each scope ---
        c("option-global-set", "remote-option%d-global-set" % family,
          {"remote": REMOTE, "server-tags": st, "options": [{"name": optname, "data": optdata}]})
        c("option-global-get", "remote-option%d-global-get" % family,
          {"remote": REMOTE, "server-tags": st, "options": [{"code": optcode}]})
        c("option-global-get-all", "remote-option%d-global-get-all" % family,
          {"remote": REMOTE, "server-tags": st})
        c("option-subnet-set", "remote-option%d-subnet-set" % family,
          {"remote": REMOTE, "subnets": [{"id": sid}], "options": [{"name": optname, "data": optdata}]})
        c("option-pool-set", "remote-option%d-pool-set" % family,
          {"remote": REMOTE, "pools": [{"pool": pool}], "options": [{"name": optname, "data": optdata}]})
        c("option-network-set", "remote-option%d-network-set" % family,
          {"remote": REMOTE, "shared-networks": [{"name": net}], "options": [{"name": optname, "data": optdata}]})
        if family == 6:
            # PD pool + pd-pool-scoped option (v6 only)
            d.call("remote-subnet6-set",
                   {"remote": REMOTE, "server-tags": st, "subnets": [
                       {"id": sid, "subnet": prefix, "shared-network-name": None,
                        "pd-pools": [{"prefix": "2001:db8:abcd::", "prefix-len": 48, "delegated-len": 56}]}]})
            c("option-pd-pool-set", "remote-option6-pd-pool-set",
              {"remote": REMOTE, "pd-pools": [{"prefix": "2001:db8:abcd::", "prefix-len": 48}],
               "options": [{"name": optname, "data": optdata}]})
        # --- integration: daemon fetches + serves the CB subnet ---
        time.sleep(4)
        _, _, cfg = d.call("config-get", {})
        served = [s for s in cfg.get("Dhcp%d" % family, {}).get("subnet%d" % family, [])
                  if s.get("id") == sid]
        ok = bool(served)
        results.append((family, "INTEGRATION daemon serves CB subnet", ok, "served=%s" % ok))
        print("  %-4s %-44s served=%s" % ("PASS" if ok else "DEV!", "daemon serves CB subnet", ok))
        # server-del in the ARM `servers` list form (probe; teardown re-dels below)
        c("server-del [ARM servers-list form]", "remote-server%d-del" % family,
          {"remote": REMOTE, "servers": [{"server-tag": "conf%d" % family}]})
        # --- teardown (best-effort; uses ARM-canonical request forms) ---
        for cmd, args in [
            ("remote-option%d-network-del" % family, {"remote": REMOTE, "shared-networks": [{"name": net}], "options": [{"code": optcode}]}),
            ("remote-option%d-pool-del" % family, {"remote": REMOTE, "pools": [{"pool": pool}], "options": [{"code": optcode}]}),
            ("remote-option%d-subnet-del" % family, {"remote": REMOTE, "subnets": [{"id": sid}], "options": [{"code": optcode}]}),
            ("remote-option%d-global-del" % family, {"remote": REMOTE, "server-tags": st, "options": [{"code": optcode}]}),
            ("remote-option-def%d-del" % family, {"remote": REMOTE, "server-tags": st, "option-defs": [{"code": 222, "space": space}]}),
            ("remote-global-parameter%d-del" % family, {"remote": REMOTE, "server-tags": st, "parameters": ["renew-timer"]}),
            ("remote-network%d-del" % family, {"remote": REMOTE, "shared-networks": [{"name": net}]}),
            ("remote-subnet%d-del-by-id" % family, {"remote": REMOTE, "subnets": [{"id": sid}]}),
            ("remote-server%d-del" % family,
             {"remote": REMOTE, "servers": [{"server-tag": "conf%d" % family}]}),
        ]:
            try:
                d.call(cmd, args)
            except Exception:
                pass


def main():
    global RUN_DIR, PRESERVE_RUN_DIR
    if not DB["password"]:
        password_names = ", ".join("%s_PASSWORD" % p for p in BACKEND_CONFIG["env_prefixes"])
        sys.exit("%s password is required (%s)" % (BACKEND_CONFIG["label"], password_names))

    requested_run_dir = os.environ.get("KEA_CB_CMDS_ARM_CONFORMANCE_RUN_DIR")
    print("ARM conformance backend: %s" % BACKEND)
    if requested_run_dir:
        RUN_DIR = os.path.abspath(requested_run_dir)
        PRESERVE_RUN_DIR = True
        os.makedirs(RUN_DIR, exist_ok=True)
        print("ARM conformance run directory: %s" % RUN_DIR)
        run_family(4, int(os.environ.get("CB_PORT4", "18010")))
        run_family(6, int(os.environ.get("CB_PORT6", "18011")))
    else:
        with tempfile.TemporaryDirectory(prefix="armconf-", dir="/tmp") as run_dir:
            RUN_DIR = run_dir
            print("ARM conformance run directory: %s" % RUN_DIR)
            run_family(4, int(os.environ.get("CB_PORT4", "18010")))
            run_family(6, int(os.environ.get("CB_PORT6", "18011")))

    devs = [r for r in results if not r[2]]
    print("\n=== SUMMARY ===")
    print("checks: %d   passed: %d   DEVIATIONS: %d"
          % (len(results), len(results) - len(devs), len(devs)))
    for fam, label, _ok, detail in devs:
        print("  DEV  v%d  %-44s %s" % (fam, label, detail))
    sys.exit(1 if devs else 0)


if __name__ == "__main__":
    main()
