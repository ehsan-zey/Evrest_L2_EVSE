import React, { useState, useCallback, useEffect } from 'react';
import { View, Text, ScrollView, RefreshControl, StyleSheet, Alert } from 'react-native';
import { useFocusEffect } from '@react-navigation/native';
import {
  api, ApiError, type ChargerDetail, type Tag, type User,
} from '../lib/api';
import { useTheme, spacing, radius } from '../lib/theme';
import {
  Card, Body, Label, Button, StatusPill, Metric,
  ErrorBanner, CurrentMeter,
} from '../components/ui';
import {
  formatEnergy, formatPower, formatAmps, formatDuration, estimateTimeFor,
} from '../lib/format';

export function ChargerDetailScreen({ route, user }: { route: any; user: User }) {
  const t = useTheme();
  const id: string = route.params.id;

  const [charger, setCharger] = useState<ChargerDetail | null>(null);
  const [tags, setTags] = useState<Tag[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [notice, setNotice] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [refreshing, setRefreshing] = useState(false);

  const load = useCallback(async () => {
    try {
      setCharger(await api.get<ChargerDetail>(`/api/chargers/${id}`));
      setError(null);
    } catch (err) {
      setError(err instanceof ApiError ? err.message : 'Could not load this charger');
    }
  }, [id]);

  useEffect(() => {
    // The driver's own tags decide what they can start a session with.
    void api.get<Tag[]>('/api/tags').then(setTags).catch(() => { /* non-fatal */ });
  }, []);

  /*
   * Poll faster than the chargers list: this is the screen someone stares at
   * while waiting for their car to start drawing current, and a stale reading
   * there reads as "it isn't working".
   */
  useFocusEffect(useCallback(() => {
    void load();
    const timer = setInterval(() => void load(), 3000);
    return () => clearInterval(timer);
  }, [load]));

  async function act(label: string, fn: () => Promise<any>): Promise<void> {
    setBusy(true);
    setError(null);
    setNotice(null);
    try {
      const res = await fn();
      // Surface what the charger actually replied. "Rejected" is a successful
      // round trip and must not look like success.
      if (res?.status && res.status !== 'Accepted') {
        setError(`${label} was ${String(res.status).toLowerCase()} by the charger`);
      } else {
        setNotice(`${label} sent — the charger is responding`);
      }
      setTimeout(() => void load(), 1500);
    } catch (err) {
      setError(err instanceof ApiError ? err.message : String(err));
    } finally {
      setBusy(false);
    }
  }

  function start(): void {
    const usable = tags.filter((tag) => tag.status === 'Accepted');
    if (usable.length === 0) {
      setError('You have no charging tag assigned. Ask your operator to add one.');
      return;
    }
    if (usable.length === 1) {
      void act('Start', () => api.post(`/api/chargers/${id}/start`, { idTag: usable[0]!.id_tag }));
      return;
    }
    // More than one tag: ask which, rather than silently picking the first.
    Alert.alert('Which tag?', 'Choose the tag to charge against.', [
      ...usable.slice(0, 3).map((tag) => ({
        text: tag.label ?? tag.id_tag,
        onPress: () => void act('Start', () =>
          api.post(`/api/chargers/${id}/start`, { idTag: tag.id_tag })),
      })),
      { text: 'Cancel', style: 'cancel' as const },
    ]);
  }

  function stop(): void {
    const txn = charger?.activeTransaction;
    if (!txn) return;
    Alert.alert('Stop charging?', 'The session will end and the car will stop drawing power.', [
      { text: 'Keep charging', style: 'cancel' },
      {
        text: 'Stop', style: 'destructive',
        onPress: () => void act('Stop', () =>
          api.post(`/api/chargers/${id}/stop`, { transactionId: txn.id })),
      },
    ]);
  }

  if (!charger) {
    return (
      <View style={{ flex: 1, backgroundColor: t.page, padding: spacing.lg }}>
        {error ? <ErrorBanner message={error} /> : <Body muted>Loading…</Body>}
      </View>
    );
  }

  const txn = charger.activeTransaction;
  const sample = charger.latestSample;
  const sessionWh = txn && sample?.energy_wh != null
    ? Math.max(0, sample.energy_wh - txn.meter_start_wh)
    : null;

  const eta = sessionWh !== null ? estimateTimeFor(20_000, sample?.power_w ?? null) : null;
  const isCharging = charger.status === 'Charging';

  return (
    <ScrollView
      style={{ flex: 1, backgroundColor: t.page }}
      contentContainerStyle={{ padding: spacing.lg, paddingBottom: spacing.xxl * 2 }}
      refreshControl={
        <RefreshControl refreshing={refreshing} tintColor={t.textMuted}
                        onRefresh={async () => {
                          setRefreshing(true); await load(); setRefreshing(false);
                        }} />
      }
    >
      {/* The navigation header already carries the charger name, so the body
          leads with the status instead of repeating it. */}
      <View style={{ marginBottom: spacing.lg }}>
        <StatusPill status={charger.status} online={charger.online} />
      </View>

      {error && <ErrorBanner message={error} onDismiss={() => setError(null)} />}
      {notice && (
        <Card style={{ marginBottom: spacing.md, paddingVertical: spacing.md }}>
          <Body>{notice}</Body>
        </Card>
      )}

      {charger.vendor_error ? (
        <ErrorBanner message={`${charger.error_code ?? 'Fault'} — ${charger.vendor_error}`} />
      ) : null}

      {/* Live session */}
      {txn ? (
        <Card style={{ marginBottom: spacing.md }}>
          <Label style={{ marginBottom: spacing.md }}>THIS SESSION</Label>

          <View style={{ flexDirection: 'row', gap: spacing.lg, marginBottom: spacing.lg }}>
            <Metric label="Delivered" value={formatEnergy(sessionWh)}
                    sub={formatDuration(Date.now() - txn.started_at)} />
            <Metric label="Power" value={formatPower(sample?.power_w)}
                    sub={eta ? `~${eta} per 20 kWh` : undefined} />
          </View>

          <CurrentMeter drawn={sample?.current_a ?? null}
                        offered={sample?.offered_a ?? null}
                        max={charger.max_current_a} />

          {/*
            When the offer is below the charger's rating, say so. "Why is it
            charging slowly" is the single most common support question, and the
            answer is nearly always a load-management profile.
          */}
          {sample?.offered_a != null && sample.offered_a < charger.max_current_a - 0.5 ? (
            <Body muted style={{ marginTop: spacing.md, fontSize: 12 }}>
              Limited to {formatAmps(sample.offered_a)} of this charger's{' '}
              {charger.max_current_a} A — a site load limit is in effect.
            </Body>
          ) : null}

          {charger.status === 'SuspendedEV' ? (
            <Body muted style={{ marginTop: spacing.md, fontSize: 12 }}>
              The car has paused charging. This is normal when the battery is nearly
              full or is balancing.
            </Body>
          ) : null}
        </Card>
      ) : null}

      {/* Controls */}
      <Card style={{ marginBottom: spacing.md }}>
        <Label style={{ marginBottom: spacing.md }}>CONTROLS</Label>

        {!charger.online ? (
          <Body muted>
            This charger is offline. It has to be connected before it can be started remotely.
          </Body>
        ) : charger.status === 'Faulted' ? (
          <Body muted>
            This charger has reported a fault and will not start a session. Contact your
            operator.
          </Body>
        ) : txn ? (
          <Button title="Stop charging" variant="danger" loading={busy} onPress={stop} />
        ) : (
          <>
            <Button title="Start charging" variant="primary" loading={busy}
                    disabled={charger.status === 'Unavailable'} onPress={start} />
            <Body muted style={{ marginTop: spacing.md, fontSize: 12 }}>
              {charger.status === 'Available'
                ? 'Plug in first — the charger will begin once the car is connected and authorised.'
                : 'Waiting for the charger to become available.'}
            </Body>
          </>
        )}
      </Card>

      {/* Details */}
      <Card>
        <Label style={{ marginBottom: spacing.md }}>DETAILS</Label>
        <Row label="Charge point" value={charger.id} theme={t} />
        <Row label="Maximum current" value={`${charger.max_current_a} A`} theme={t} />
        {sample?.voltage_v != null && (
          <Row label="Supply voltage" value={`${sample.voltage_v.toFixed(0)} V`} theme={t} />
        )}
        {txn && <Row label="Session" value={`#${txn.id}`} theme={t} />}
        {txn && <Row label="Tag" value={txn.id_tag} theme={t} last />}
      </Card>

      {user.role !== 'driver' ? (
        <Body muted style={{ marginTop: spacing.lg, fontSize: 12, textAlign: 'center' }}>
          Signed in as {user.role}. Resets and limits are available in the web dashboard.
        </Body>
      ) : null}
    </ScrollView>
  );
}

function Row({ label, value, theme, last }: {
  label: string; value: string; theme: any; last?: boolean;
}) {
  return (
    <View style={{
      flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center',
      paddingVertical: spacing.md,
      borderBottomWidth: last ? 0 : StyleSheet.hairlineWidth,
      borderBottomColor: theme.border,
    }}>
      <Text style={{ fontSize: 14, color: theme.textSecondary }}>{label}</Text>
      <Text style={{ fontSize: 14, color: theme.textPrimary, fontWeight: '500' }}>{value}</Text>
    </View>
  );
}
