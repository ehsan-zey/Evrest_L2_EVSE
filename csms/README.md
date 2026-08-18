# EVREST CSMS

OCPP 1.6J Central System, plus the REST and live-WebSocket API the dashboard and
mobile app run on. Node 20+, TypeScript, SQLite.

## Running

```sh
npm install
cp .env.example .env      # then edit it -- JWT_SECRET must be changed
npm run dev               # or: npm run build && npm start
```

On an empty database an administrator account is created and its password
printed **once**. Change it after signing in.

## Ports

| Port | Who connects | What it speaks |
|------|--------------|----------------|
| `PORT` (9220) | Charge points | OCPP 1.6J over WebSocket, subprotocol `ocpp1.6` |
| `API_PORT` (9221) | Dashboard, mobile app | REST + a `/live` WebSocket |

Two ports rather than one so they can be firewalled differently. The charger
port is reachable from field devices on networks nobody controls; the API port
should not be.

Anything that is not a WebSocket upgrade on the OCPP port gets a `426 Upgrade
Required`, so a misconfigured monitor pointed at it fails loudly.

## Charger authentication

Chargers authenticate with HTTP Basic at the WebSocket upgrade — OCPP security
profile 1 (or 2 behind TLS). The charge point id is the Basic username and must
match the last path segment:

```
wss://csms.example.com:9220/ocpp/EVREST-0001
Authorization: Basic base64("EVREST-0001:<key>")
```

Rejection happens **during the upgrade**, as a real HTTP 401. Closing the socket
after a successful handshake would leave the charger believing it had connected
and retrying OCPP messages forever instead of surfacing the failure.

Provision a charger with `POST /api/chargers`; the key is returned once and only
its SHA-256 is stored. A charger that boots without being provisioned is
registered automatically but gets **no** key, so with `REQUIRE_CHARGER_AUTH=true`
it cannot connect again until an operator accepts it. Commissioning is a
deliberate two-step: the charger announces itself, a human approves it.

## API

All routes except `/api/health` and `/api/auth/login` need
`Authorization: Bearer <jwt>`.

Roles are ranked `driver < operator < admin`, so an operator route is
automatically open to admins.

| Method | Path | Role | Notes |
|---|---|---|---|
| POST | `/api/auth/login` | — | returns a 12 h token |
| GET | `/api/auth/me` | any | |
| POST | `/api/users` | admin | |
| GET | `/api/chargers` | any | includes the live session state |
| GET | `/api/chargers/:id` | any | + active transaction and latest sample |
| POST | `/api/chargers` | admin | returns the auth key once |
| POST | `/api/chargers/:id/rotate-key` | admin | |
| POST | `/api/chargers/:id/start` | any | drivers may only use their own tag |
| POST | `/api/chargers/:id/stop` | any | drivers may only stop their own session |
| POST | `/api/chargers/:id/reset` | operator | `Soft` or `Hard` |
| POST | `/api/chargers/:id/availability` | operator | |
| POST | `/api/chargers/:id/limit` | operator | amps; `0` clears the profile |
| POST | `/api/chargers/:id/trigger` | operator | TriggerMessage |
| GET/POST | `/api/chargers/:id/config` | operator | Get/ChangeConfiguration |
| GET | `/api/chargers/:id/log` | operator | raw OCPP frames |
| GET | `/api/chargers/:id/samples` | any | meter history for charts |
| GET | `/api/transactions` | any | drivers see only their own |
| GET | `/api/transactions/:id/samples` | any | |
| GET/POST/DELETE | `/api/tags` | operator | |
| GET | `/api/stats/summary` | any | fleet totals and a 30-day series |

`auth_key_hash` is stripped from every charger response. It is only a digest,
but there is no reason for a browser to hold it.

### Live updates

```
ws://host:9221/live?token=<jwt>
```

Send `{"type":"subscribe","chargers":["EVREST-0001"]}` to narrow the feed; an
empty or absent filter means everything. Events:

`charger.online`, `charger.status`, `charger.boot`, `charger.firmware`,
`transaction.start`, `transaction.stop`, `meter.values`, `ocpp.message`.

`ocpp.message` carries raw frames — which can contain idTags and configuration —
so it is withheld from `driver` clients.

## Tests

```sh
npm test        # 29 integration tests: a fake charger over a real WebSocket
npm run typecheck
```

The tests drive a real WebSocket against the real server with an in-memory
database, rather than calling handlers directly. That is what caught the
original bug where an unauthenticated charger's handshake completed before it
was rejected.

Cases covered include: wrong/absent/unknown credentials, path traversal in the
charge point id, duplicate sessions, schema violations and over-length CiStrings,
the full transaction lifecycle, a meter register that runs backwards, a charger
rebooting mid-session, double stops, stops for unknown transactions,
`connectorId 0` not clobbering connector state, server-initiated calls, calls to
an offline charger, malformed frames, and event-bus isolation.

## Design notes

**Handlers are synchronous.** better-sqlite3 is synchronous, so a handler that
awaited anything could interleave with the next frame from the same charger and
reorder a `StartTransaction` against its `StopTransaction`. Keeping them
synchronous makes per-charger ordering free rather than something to reason
about.

**Unknown tags are `Invalid`.** Defaulting the other way is how a network gives
away energy. The only way to charge is to be in the `tags` table.

**Liveness uses the negotiated heartbeat interval**, not a fixed timeout. An
operator who lengthens the heartbeat on a metered cellular link should not find
their chargers marked offline every minute.

**A charger going offline resets its status.** Leaving the last-known
`Charging` is how a dashboard ends up showing a session that ended hours ago.

**Stale transactions are closed on a new start.** A charger that reboots
mid-session otherwise leaves two open transactions on one connector, which
corrupts every energy total it reports afterwards.
