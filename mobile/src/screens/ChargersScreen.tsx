import React, { useState, useCallback } from 'react';
import { View, Text, FlatList, RefreshControl, Pressable, StyleSheet } from 'react-native';
import { useFocusEffect } from '@react-navigation/native';
import { api, ApiError, type Charger } from '../lib/api';
import { useTheme, spacing, radius } from '../lib/theme';
import { StatusPill, ErrorBanner, Empty, Title, Body } from '../components/ui';

/** Chargers a driver can act on come first; broken and offline ones sink. */
function sortKey(c: Charger): number {
  if (c.status === 'Charging')  return 0;
  if (!c.online)                return 4;
  if (c.status === 'Faulted')   return 3;
  if (c.status === 'Available') return 1;
  return 2;
}

export function ChargersScreen({ navigation }: { navigation: any }) {
  const t = useTheme();
  const [chargers, setChargers] = useState<Charger[]>([]);
  const [error, setError] = useState<string | null>(null);
  const [refreshing, setRefreshing] = useState(false);

  const load = useCallback(async () => {
    try {
      const rows = await api.get<Charger[]>('/api/chargers');
      setChargers([...rows].sort((a, b) => sortKey(a) - sortKey(b) || a.name.localeCompare(b.name)));
      setError(null);
    } catch (err) {
      setError(err instanceof ApiError ? err.message : 'Could not load chargers');
    }
  }, []);

  /*
   * Refresh whenever the screen comes into focus and poll while it is visible.
   * A five-second poll is cheap and means a driver who walks up to a bay does
   * not have to pull to refresh to see it turn Available.
   */
  useFocusEffect(useCallback(() => {
    void load();
    const timer = setInterval(() => void load(), 5000);
    return () => clearInterval(timer);
  }, [load]));

  return (
    <View style={{ flex: 1, backgroundColor: t.page }}>
      <FlatList
        data={chargers}
        keyExtractor={(c) => c.id}
        contentContainerStyle={{ padding: spacing.lg, paddingBottom: spacing.xxl * 2 }}
        refreshControl={
          <RefreshControl refreshing={refreshing} tintColor={t.textMuted}
                          onRefresh={async () => {
                            setRefreshing(true);
                            await load();
                            setRefreshing(false);
                          }} />
        }
        ListHeaderComponent={
          <View style={{ marginBottom: spacing.lg }}>
            <Title>Chargers</Title>
            <Body muted style={{ marginTop: 2 }}>
              {chargers.filter((c) => c.online).length} of {chargers.length} online
            </Body>
            {error ? <View style={{ marginTop: spacing.md }}>
              <ErrorBanner message={error} onDismiss={() => setError(null)} />
            </View> : null}
          </View>
        }
        ListEmptyComponent={
          error ? null : <Empty text="No chargers are registered on this server yet." />
        }
        renderItem={({ item }) => (
          <Pressable
            onPress={() => navigation.navigate('ChargerDetail', { id: item.id, name: item.name })}
            style={({ pressed }) => ({
              backgroundColor: t.surface,
              borderRadius: radius.md,
              borderWidth: StyleSheet.hairlineWidth,
              borderColor: t.border,
              padding: spacing.lg,
              marginBottom: spacing.md,
              opacity: pressed ? 0.8 : 1,
            })}
            accessibilityRole="button"
            accessibilityLabel={`${item.name}, ${item.online ? item.status : 'offline'}`}
          >
            <View style={{ flexDirection: 'row', justifyContent: 'space-between',
                           alignItems: 'flex-start', gap: spacing.md }}>
              <View style={{ flex: 1 }}>
                <Text style={{ fontSize: 17, fontWeight: '600', color: t.textPrimary }}>
                  {item.name}
                </Text>
                <Text style={{ fontSize: 12, color: t.textMuted, marginTop: 2 }}>
                  {item.id} · up to {item.max_current_a} A
                </Text>
              </View>
              <StatusPill status={item.status} online={item.online} />
            </View>

            {item.vendor_error ? (
              <Text style={{ fontSize: 12, color: t.critical, marginTop: spacing.sm }}>
                ▲ {item.vendor_error}
              </Text>
            ) : null}

            {item.active_transaction_id ? (
              <Text style={{ fontSize: 12, color: t.textSecondary, marginTop: spacing.sm }}>
                In use since {new Date(item.active_started_at!).toLocaleTimeString([], {
                  hour: '2-digit', minute: '2-digit' })}
              </Text>
            ) : null}
          </Pressable>
        )}
      />
    </View>
  );
}
