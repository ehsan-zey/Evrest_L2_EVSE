/**
 * Handlers for messages a charge point sends us.
 *
 * Every handler is synchronous and returns the confirmation payload. That is
 * deliberate: better-sqlite3 is synchronous, so a handler that touched the
 * database and then awaited something could interleave with the next message
 * from the same charger and reorder a StartTransaction against its
 * StopTransaction. Keeping them synchronous makes per-charger ordering free.
 */
import type { OcppServer, ChargerSession } from './server.js';
import type { InboundAction, IdTagInfo } from './types.js';
import { ocppNow } from './types.js';
import type { TagRow, ChargerStatus } from '../db/schema.js';

/** Parse an OCPP timestamp, falling back to now if the charger sent nonsense. */
function parseTs(iso: string | undefined): number {
  if (!iso) return Date.now();
  const t = Date.parse(iso);
  return Number.isFinite(t) ? t : Date.now();
}

/**
 * Resolve an idTag against the tag table.
 *
 * An unknown tag is Invalid, not Accepted. Defaulting the other way is how a
 * charge network ends up giving away energy — so the only way to charge is to
 * be in the table.
 */
function authoriseTag(server: OcppServer, idTag: string): IdTagInfo {
  const row = server.db
    .prepare('SELECT * FROM tags WHERE id_tag = ?')
    .get(idTag) as TagRow | undefined;

  if (!row) return { status: 'Invalid' };

  if (row.status !== 'Accepted') {
    return { status: row.status };
  }
  if (row.expires_at !== null && row.expires_at < Date.now()) {
    return { status: 'Expired' };
  }

  const info: IdTagInfo = { status: 'Accepted' };
  if (row.parent_id_tag) info.parentIdTag = row.parent_id_tag;
  if (row.expires_at !== null) info.expiryDate = new Date(row.expires_at).toISOString();
  return info;
}

/** Pull a named measurand out of a MeterValues sample set. */
function measurand(samples: Array<{ value: string; measurand?: string }>,
                   name: string, defaultName = false): number | null {
  for (const s of samples) {
    const m = s.measurand ?? (defaultName ? 'Energy.Active.Import.Register' : undefined);
    if (m === name) {
      const v = Number.parseFloat(s.value);
      if (Number.isFinite(v)) return v;
    }
  }
  return null;
}

export function handleInbound(
  server: OcppServer,
  session: ChargerSession,
  action: InboundAction,
  payload: any,
): Record<string, unknown> {
  const db = server.db;
  const chargerId = session.chargerId;

  switch (action) {

  /* ---------------------------------------------------------------- */
  case 'BootNotification': {
    const now = Date.now();
    const existing = db.prepare('SELECT id, heartbeat_interval FROM chargers WHERE id = ?')
      .get(chargerId) as { id: string; heartbeat_interval: number } | undefined;

    if (existing) {
      db.prepare(
        `UPDATE chargers
            SET vendor = ?, model = ?, serial = ?, firmware = ?,
                last_boot = ?, last_seen = ?, online = 1
          WHERE id = ?`,
      ).run(payload.chargePointVendor, payload.chargePointModel,
            payload.chargePointSerialNumber ?? null,
            payload.firmwareVersion ?? null, now, now, chargerId);
      session.heartbeatInterval = existing.heartbeat_interval;
    } else {
      /*
       * An unknown charger is registered automatically but NOT given an auth
       * key, so with REQUIRE_CHARGER_AUTH on it cannot connect again until an
       * operator provisions one. That makes commissioning a two-step process
       * on purpose: the charger announces itself, a human accepts it.
       */
      db.prepare(
        `INSERT INTO chargers (id, name, vendor, model, serial, firmware,
                               status, online, last_boot, last_seen, created_at)
         VALUES (?, ?, ?, ?, ?, ?, 'Available', 1, ?, ?, ?)`,
      ).run(chargerId, chargerId, payload.chargePointVendor, payload.chargePointModel,
            payload.chargePointSerialNumber ?? null, payload.firmwareVersion ?? null,
            now, now, now);
      server.log(`${chargerId}: registered on first boot`);
    }

    server.events.emit('charger.boot', { chargerId, at: now });

    return {
      status: 'Accepted',
      currentTime: ocppNow(),
      interval: session.heartbeatInterval,
    };
  }

  /* ---------------------------------------------------------------- */
  case 'Heartbeat': {
    db.prepare('UPDATE chargers SET last_seen = ?, online = 1 WHERE id = ?')
      .run(Date.now(), chargerId);
    return { currentTime: ocppNow() };
  }

  /* ---------------------------------------------------------------- */
  case 'StatusNotification': {
    // connectorId 0 refers to the charge point as a whole rather than an
    // outlet. Recording it as the connector status would make a charge-point
    // level notice overwrite a live session's state.
    if (payload.connectorId === 0) {
      if (payload.errorCode !== 'NoError') {
        db.prepare('UPDATE chargers SET error_code = ?, vendor_error = ? WHERE id = ?')
          .run(payload.errorCode, payload.vendorErrorCode ?? null, chargerId);
      }
      return {};
    }

    db.prepare(
      `UPDATE chargers SET status = ?, error_code = ?, vendor_error = ?, last_seen = ?
        WHERE id = ?`,
    ).run(payload.status as ChargerStatus,
          payload.errorCode === 'NoError' ? null : payload.errorCode,
          payload.vendorErrorCode ?? null, Date.now(), chargerId);

    server.events.emit('charger.status', {
      chargerId,
      status: payload.status,
      errorCode: payload.errorCode,
      vendorErrorCode: payload.vendorErrorCode ?? null,
      at: parseTs(payload.timestamp),
    });
    return {};
  }

  /* ---------------------------------------------------------------- */
  case 'Authorize': {
    return { idTagInfo: authoriseTag(server, payload.idTag) };
  }

  /* ---------------------------------------------------------------- */
  case 'StartTransaction': {
    const info = authoriseTag(server, payload.idTag);

    if (info.status !== 'Accepted') {
      // Still return a transactionId: OCPP 1.6 requires one, and the charger
      // needs it to send the StopTransaction it is about to send.
      return { transactionId: 0, idTagInfo: info };
    }

    /*
     * A charger that reboots mid-session sends a fresh StartTransaction while
     * an old one is still open. Close the stale one rather than leaving two
     * open transactions on one connector, which would corrupt every energy
     * total that charger reports afterwards.
     */
    const stale = db.prepare(
      'SELECT id, meter_start_wh FROM transactions WHERE charger_id = ? AND stopped_at IS NULL',
    ).all(chargerId) as Array<{ id: number; meter_start_wh: number }>;

    for (const s of stale) {
      db.prepare(
        `UPDATE transactions
            SET stopped_at = ?, stop_reason = 'PowerLoss',
                meter_stop_wh = ?, energy_wh = 0
          WHERE id = ?`,
      ).run(Date.now(), s.meter_start_wh, s.id);
      server.log(`${chargerId}: closed stale transaction ${s.id}`);
    }

    const startedAt = parseTs(payload.timestamp);
    const result = db.prepare(
      `INSERT INTO transactions (charger_id, connector_id, id_tag, meter_start_wh, started_at)
       VALUES (?, ?, ?, ?, ?)`,
    ).run(chargerId, payload.connectorId, payload.idTag, payload.meterStart, startedAt);

    const transactionId = Number(result.lastInsertRowid);
    server.events.emit('transaction.start', {
      chargerId, transactionId, idTag: payload.idTag, at: startedAt,
    });

    return { transactionId, idTagInfo: info };
  }

  /* ---------------------------------------------------------------- */
  case 'StopTransaction': {
    const txn = db.prepare('SELECT * FROM transactions WHERE id = ?')
      .get(payload.transactionId) as
      { id: number; charger_id: string; meter_start_wh: number; stopped_at: number | null } | undefined;

    const info = payload.idTag ? authoriseTag(server, payload.idTag) : undefined;

    if (!txn) {
      // A replayed StopTransaction for a transaction we never saw — usually a
      // charger that was offline through the whole session. Nothing to update,
      // but acknowledging it stops the charger retrying forever.
      server.log(`${chargerId}: stop for unknown transaction ${payload.transactionId}`);
      return info ? { idTagInfo: info } : {};
    }
    if (txn.charger_id !== chargerId) {
      // One charger must not be able to close another's transaction.
      server.log(`${chargerId}: refused stop for transaction ${txn.id} owned by ${txn.charger_id}`);
      return info ? { idTagInfo: info } : {};
    }
    if (txn.stopped_at !== null) {
      return info ? { idTagInfo: info } : {};   // already closed; idempotent
    }

    const stoppedAt = parseTs(payload.timestamp);
    // Clamp at zero: a meter register that appears to go backwards (a charger
    // reset, a replaced meter) must not produce negative billable energy.
    const energy = Math.max(0, payload.meterStop - txn.meter_start_wh);

    db.prepare(
      `UPDATE transactions
          SET meter_stop_wh = ?, stopped_at = ?, stop_reason = ?, energy_wh = ?
        WHERE id = ?`,
    ).run(payload.meterStop, stoppedAt, payload.reason ?? 'Local', energy, txn.id);

    server.events.emit('transaction.stop', {
      chargerId, transactionId: txn.id, energyWh: energy,
      reason: payload.reason ?? 'Local', at: stoppedAt,
    });

    return info ? { idTagInfo: info } : {};
  }

  /* ---------------------------------------------------------------- */
  case 'MeterValues': {
    const insert = db.prepare(
      `INSERT INTO meter_samples
         (charger_id, transaction_id, ts, energy_wh, power_w, current_a, voltage_v, offered_a)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
    );

    // Resolve the transaction once: the charger may omit transactionId, and
    // looking up the open transaction per sample would be wasteful.
    let txnId: number | null = payload.transactionId ?? null;
    if (txnId === null) {
      const open = db.prepare(
        'SELECT id FROM transactions WHERE charger_id = ? AND stopped_at IS NULL',
      ).get(chargerId) as { id: number } | undefined;
      txnId = open?.id ?? null;
    }

    const latest: Record<string, number | null> = {};

    const tx = db.transaction(() => {
      for (const mv of payload.meterValue) {
        const ts = parseTs(mv.timestamp);
        const s = mv.sampledValue;

        // A sampledValue with no measurand means the register, per OCPP 1.6.
        const energy  = measurand(s, 'Energy.Active.Import.Register', true);
        const power   = measurand(s, 'Power.Active.Import');
        const current = measurand(s, 'Current.Import');
        const voltage = measurand(s, 'Voltage');
        const offered = measurand(s, 'Current.Offered');

        insert.run(chargerId, txnId, ts, energy, power, current, voltage, offered);

        latest.energyWh  = energy;
        latest.powerW    = power;
        latest.currentA  = current;
        latest.voltageV  = voltage;
        latest.offeredA  = offered;
        latest.ts        = ts;
      }
    });
    tx();

    server.events.emit('meter.values', { chargerId, transactionId: txnId, ...latest });
    return {};
  }

  /* ---------------------------------------------------------------- */
  case 'DataTransfer':
    // No vendor extensions are implemented. Saying so is better than a bare
    // "Accepted" that quietly discards whatever the charger sent.
    return { status: 'UnknownVendorId' };

  case 'DiagnosticsStatusNotification':
  case 'FirmwareStatusNotification':
    server.events.emit('charger.firmware', { chargerId, status: payload.status });
    return {};
  }

  // Unreachable: the caller checked the action against INBOUND_SCHEMAS.
  throw new Error(`Unhandled action ${action}`);
}
