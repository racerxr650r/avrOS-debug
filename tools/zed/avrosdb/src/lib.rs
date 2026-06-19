// avrOSdb (DAP) — Zed debug-adapter extension.
//
// Zed speaks the Debug Adapter Protocol but, like VS Code, will not talk to an
// arbitrary DAP server without a contributed adapter type. This extension
// registers the `avrosdb` adapter so Zed can start a debug session against
// `avrOSdb --dap` with no hand-written wiring.
//
// avrOSdb serves DAP over a TCP socket, so:
//
//   * request: "launch"  → Zed spawns `avrOSdb --dap --port <port> <serial>
//                          <elf>` (plus any extraArgs) and connects to it over
//                          127.0.0.1:<port> — one action starts the server and
//                          attaches.
//   * request: "attach"  → Zed connects to an already-running server (no spawn),
//                          using the config's host/port (or a `server`
//                          "host:port" string).
//
// Unlike the VS Code companion (tools/vscode/avrosdb-dap), this extension does
// NOT surface avrOS state-machine / event / queue introspection: Zed does not
// let an extension contribute the bespoke tree views that drive the
// avrosdb/fsmList|eventList|queueList custom requests. The Zed integration is
// therefore launch/attach plus Zed's built-in Variables / Call Stack /
// Breakpoints panes. See doc/UserManual.md §6.4 and HLR-090.

use std::{net::Ipv4Addr, time::Duration};

use zed_extension_api::{
    self as zed, DebugAdapterBinary, DebugConfig, DebugRequest, DebugScenario, DebugTaskDefinition,
    StartDebuggingRequestArguments, StartDebuggingRequestArgumentsRequest, TcpArguments, Worktree,
    serde_json,
};

const ADAPTER_NAME: &str = "avrosdb";

// How long Zed waits to connect to the spawned server's TCP socket. avrOSdb
// binds its listener at start-up, well before the (slower) UPDI link comes up,
// so a few seconds is ample headroom.
const DEFAULT_TIMEOUT: Duration = Duration::from_secs(8);

// Defaults mirror the VS Code companion so the two front-ends behave alike.
const DEFAULT_PORT: u16 = 1234;
const DEFAULT_SERIAL: &str = "/dev/ttyAMA2";
const DEFAULT_ELF: &str = "firmware.elf";

fn verify_adapter_name(adapter_name: &str) -> Result<(), String> {
    if adapter_name != ADAPTER_NAME {
        Err(format!(
            "Unsupported debug adapter name '{adapter_name}', expected '{ADAPTER_NAME}'"
        ))
    } else {
        Ok(())
    }
}

fn config_port(cfg: &serde_json::Value) -> u16 {
    cfg.get("port")
        .and_then(|p| p.as_u64())
        .and_then(|p| u16::try_from(p).ok())
        .unwrap_or(DEFAULT_PORT)
}

fn config_host(cfg: &serde_json::Value) -> u32 {
    cfg.get("host")
        .and_then(|h| h.as_str())
        .and_then(|h| h.parse::<Ipv4Addr>().ok())
        .unwrap_or(Ipv4Addr::LOCALHOST)
        .to_bits()
}

struct AvrOsDbDebugExtension;

impl zed::Extension for AvrOsDbDebugExtension {
    fn new() -> Self
    where
        Self: Sized,
    {
        Self
    }

    fn get_dap_binary(
        &mut self,
        adapter_name: String,
        config: DebugTaskDefinition,
        user_provided_debug_adapter_path: Option<String>,
        _worktree: &Worktree,
    ) -> Result<DebugAdapterBinary, String> {
        verify_adapter_name(&adapter_name)?;

        let json_config: serde_json::Value = serde_json::from_str(&config.config)
            .map_err(|err| format!("Failed to parse avrosdb debug config: {err}"))?;

        let request = json_config
            .get("request")
            .and_then(|r| r.as_str())
            .unwrap_or("launch");

        // An explicit `server: "host:port"` string always means connect-only,
        // regardless of request — it points at an externally-started server.
        let server_connection = json_config
            .get("server")
            .and_then(|s| s.as_str())
            .map(parse_server_string)
            .transpose()?
            .map(|mut tcp| {
                tcp.timeout = Some(DEFAULT_TIMEOUT.as_millis() as u64);
                tcp
            });

        let port = config_port(&json_config);
        let host = config_host(&json_config);

        let (command, arguments, connection) = if server_connection.is_some() || request == "attach"
        {
            // Attach: connect to a server someone else started; spawn nothing.
            let connection = server_connection.unwrap_or(TcpArguments {
                port,
                host,
                timeout: Some(DEFAULT_TIMEOUT.as_millis() as u64),
            });
            (None, Vec::new(), connection)
        } else {
            // Launch: spawn `avrOSdb --dap --port <port> [extraArgs] <serial>
            // <elf>` and dial its socket.
            let program = json_config
                .get("program")
                .and_then(|p| p.as_str())
                .map(str::to_string)
                .or(user_provided_debug_adapter_path)
                .unwrap_or_else(|| "avrOSdb".to_string());

            let serial = json_config
                .get("serial")
                .and_then(|s| s.as_str())
                .unwrap_or(DEFAULT_SERIAL)
                .to_string();
            let elf = json_config
                .get("elf")
                .and_then(|e| e.as_str())
                .unwrap_or(DEFAULT_ELF)
                .to_string();

            let mut arguments = vec!["--dap".to_string(), "--port".to_string(), port.to_string()];
            if let Some(extra) = json_config.get("extraArgs").and_then(|a| a.as_array()) {
                for a in extra {
                    if let Some(s) = a.as_str() {
                        arguments.push(s.to_string());
                    }
                }
            }
            // Positional operands: serial device, then ELF.
            arguments.push(serial);
            arguments.push(elf);

            let connection = TcpArguments {
                port,
                host,
                timeout: Some(DEFAULT_TIMEOUT.as_millis() as u64),
            };
            (Some(program), arguments, connection)
        };

        let dap_request = match request {
            "attach" => StartDebuggingRequestArgumentsRequest::Attach,
            _ => StartDebuggingRequestArgumentsRequest::Launch,
        };

        Ok(DebugAdapterBinary {
            command,
            arguments,
            envs: vec![],
            cwd: json_config
                .get("cwd")
                .and_then(|c| c.as_str())
                .map(str::to_string),
            connection: Some(connection),
            request_args: StartDebuggingRequestArguments {
                configuration: config.config,
                request: dap_request,
            },
        })
    }

    fn dap_request_kind(
        &mut self,
        adapter_name: String,
        config: serde_json::Value,
    ) -> Result<StartDebuggingRequestArgumentsRequest, String> {
        verify_adapter_name(&adapter_name)?;

        // Default to launch when unspecified (the common case).
        match config.get("request").and_then(|f| f.as_str()) {
            Some("attach") => Ok(StartDebuggingRequestArgumentsRequest::Attach),
            Some("launch") | None => Ok(StartDebuggingRequestArgumentsRequest::Launch),
            Some(other) => Err(format!(
                "Invalid 'request' value '{other}'; only 'launch' and 'attach' are supported"
            )),
        }
    }

    fn dap_config_to_scenario(&mut self, debug_config: DebugConfig) -> Result<DebugScenario, String> {
        verify_adapter_name(&debug_config.adapter)?;

        let config = match debug_config.request {
            // Zed's generic "debug this program" flow: the chosen program is the
            // ELF whose DWARF drives source/variables; avrOSdb is found on PATH
            // (or via a `program` field added by hand). serial/port take the
            // shared defaults and can be edited in the generated debug.json.
            DebugRequest::Launch(launch_request) => serde_json::json!({
                "request": "launch",
                "serial": DEFAULT_SERIAL,
                "elf": launch_request.program,
                "cwd": launch_request.cwd,
                "port": DEFAULT_PORT,
            }),
            // Attach to an already-running `avrOSdb --dap` server.
            DebugRequest::Attach(_) => serde_json::json!({
                "request": "attach",
                "host": "127.0.0.1",
                "port": DEFAULT_PORT,
            }),
        };

        Ok(DebugScenario {
            label: debug_config.label,
            adapter: debug_config.adapter,
            build: None,
            config: config.to_string(),
            tcp_connection: None,
        })
    }
}

// Parse a `server` field of the form "host:port" into TcpArguments.
fn parse_server_string(server_string: &str) -> Result<TcpArguments, String> {
    let (host_str, port_str) = server_string.rsplit_once(':').ok_or_else(|| {
        format!("Invalid server string '{server_string}'. Expected 'host:port'")
    })?;

    let host: Ipv4Addr = host_str
        .parse()
        .map_err(|_| format!("Invalid IP address '{host_str}'. Expected a valid IPv4 address"))?;
    let port: u16 = port_str
        .parse()
        .map_err(|_| format!("Invalid port '{port_str}'. Expected a number between 0 and 65535"))?;

    Ok(TcpArguments {
        port,
        host: host.to_bits(),
        timeout: None,
    })
}

zed::register_extension!(AvrOsDbDebugExtension);

#[cfg(test)]
mod test {
    use super::parse_server_string;
    use std::net::Ipv4Addr;

    #[test]
    fn parse_server_string_valid() {
        let tcp = parse_server_string("127.0.0.1:1234").unwrap();
        assert_eq!(tcp.port, 1234);
        assert_eq!(tcp.host, Ipv4Addr::LOCALHOST.to_bits());
    }

    #[test]
    fn parse_server_string_rejects_bad_input() {
        assert!(parse_server_string("127.0.0.1").is_err());
        assert!(parse_server_string("localhost:1234").is_err());
        assert!(parse_server_string("127.0.0.1:abc").is_err());
        assert!(parse_server_string("127.0.0.1:70000").is_err());
    }
}
