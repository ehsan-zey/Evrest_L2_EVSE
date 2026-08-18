import React, { useState, useCallback } from 'react';
import { View, Text, FlatList, RefreshControl, StyleSheet } from 'react-native';
import { useFocusEffect } from '@react-navigation/native';
import { api, ApiError, type Transaction } from '../lib/api';
import { useTheme, spacing, radius } from '../lib/theme';
import { Title, Body, Metric, ErrorBanner, Empty } from '../components/ui';
import { formatEnergy, formatDuration, formatDateTime } from '../lib/format';

export function HistoryScreen() {
  const t = useTheme();
  const [rows, setRows] = useState<Transaction[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [refreshing, setRefreshing] = useState(false);

  const load = useCallback(async () => {
    try {
      // The API already scopes a driver to their own sessions.
      setRows(await api.get<Transaction[]>('/api/transactions?limit=100'));
      setError(null);
    } catch (err) {
      setError(err instanceof ApiError ? err.message : 'Could not load your history');
    }
  }, []);

  useFocusEffect(useCallback(() => { void load(); }, [load]));

  const completed = rows.filter((r) => r.stopped_at !== null);
  const totalWh = completed.reduce((sum, r) => sum + (r.energy_wh ?? 0), 0);
  const thisMonth = completed.filter(
    (r) => new Date(r.started_at).getMonth() === new Date().getMonth(),
  );
  const monthWh = thisMonth.reduce((sum, r) => sum + (r.energy_wh ?? 0), 0);

  return (
    <View style={{ flex: 1, backgroundColor: t.page }}>
      <FlatList
        data={rows}
        keyExtractor={(r) => String(r.id)}
        contentContainerStyle={{ padding: spacing.lg, paddingBottom: spacing.xxl * 2 }}
        refreshControl={
          <RefreshControl refreshing={refreshing} tintColor={t.textMuted}
                          onRefresh={async () => {
                            setRefreshing(true); await load(); setRefreshing(false);
                          }} />
        }
        ListHeaderComponent={
          <View style={{ marginBottom: spacing.lg }}>
            <Title>Your charging</Title>
            {error ? <View style={{ marginTop: spacing.md }}>
              <ErrorBanner message={error} onDismiss={() => setError(null)} />
            </View> : null}
            <View style={{
              flexDirection: 'row', gap: spacing.lg, marginTop: spacing.lg,
              backgroundColor: t.surface, borderRadius: radius.md, padding: spacing.lg,
              borderWidth: StyleSheet.hairlineWidth, borderColor: t.border,
            }}>
              <Metric label="This month" value={formatEnergy(monthWh)}
                      sub={`${thisMonth.length} sessions`} />
              <Metric label="All time" value={formatEnergy(totalWh)}
                      sub={`${completed.length} sessions`} />
            </View>
          </View>
        }
        ListEmptyComponent={
          error ? null : <Empty text="No charging sessions yet. They will appear here once you charge." />
        }
        renderItem={({ item }) => (
          <View style={{
            backgroundColor: t.surface, borderRadius: radius.md, padding: spacing.lg,
            marginBottom: spacing.md,
            borderWidth: StyleSheet.hairlineWidth, borderColor: t.border,
          }}>
            <View style={{ flexDirection: 'row', justifyContent: 'space-between',
                           alignItems: 'flex-start' }}>
              <View style={{ flex: 1 }}>
                <Text style={{ fontSize: 15, fontWeight: '600', color: t.textPrimary }}>
                  {item.charger_name ?? item.charger_id}
                </Text>
                <Text style={{ fontSize: 12, color: t.textMuted, marginTop: 2 }}>
                  {formatDateTime(item.started_at)}
                </Text>
              </View>
              <View style={{ alignItems: 'flex-end' }}>
                {item.stopped_at ? (
                  <>
                    <Text style={{ fontSize: 17, fontWeight: '700', color: t.textPrimary }}>
                      {formatEnergy(item.energy_wh)}
                    </Text>
                    <Text style={{ fontSize: 12, color: t.textMuted, marginTop: 2 }}>
                      {formatDuration(item.stopped_at - item.started_at)}
                    </Text>
                  </>
                ) : (
                  <Text style={{ fontSize: 13, fontWeight: '600', color: t.good }}>
                    ⚡ In progress
                  </Text>
                )}
              </View>
            </View>
          </View>
        )}
      />
    </View>
  );
}
