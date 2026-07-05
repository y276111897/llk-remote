# Security Policy

LLK Remote is an experimental LAN remote-control project. It currently has no
authentication, no encrypted transport, and no authorization model.

## Supported Use

Use it only on private networks and only between machines you own or are
authorized to test.

Do not expose the default ports to the public Internet:

- `52334/UDP`
- `52333/UDP`
- `52335/TCP`

## Reporting Issues

For now, report security issues privately to the repository owner after the
public GitHub repository is created. Do not publish working exploit details in a
public issue before the maintainer has had time to respond.

## Current Hardening Roadmap

- optional session authentication
- optional encrypted transport
- stricter packet validation and fuzz tests
- safer transfer target controls
- clearer warnings in the viewer and host logs
