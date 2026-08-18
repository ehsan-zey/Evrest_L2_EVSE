/**
 * Shared UI primitives.
 *
 * Kept in one file because they are small and always used together; splitting
 * six twenty-line components across six files buys nothing here.
 */
import React from 'react';
import {
  View, Text, StyleSheet, Pressable, ActivityIndicator,
  type ViewStyle, type TextStyle,
} from 'react-native';
import { useTheme, spacing, radius, type Theme } from '../lib/theme';
import type { ChargerStatus } from '../lib/api';

export function Card({ children, style }: { children: React.ReactNode; style?: ViewStyle }) {
  const t = useTheme();
  return (
    <View style={[{
      backgroundColor: t.surface,
      borderRadius: radius.md,
      borderWidth: StyleSheet.hairlineWidth,
      borderColor: t.border,
      padding: spacing.lg,
    }, style]}>
      {children}
    </View>
  );
}

export function Title({ children, style }: { children: React.ReactNode; style?: TextStyle }) {
  const t = useTheme();
  return <Text style={[{ fontSize: 22, fontWeight: '700', color: t.textPrimary,
                         letterSpacing: -0.3 }, style]}>{children}</Text>;
}

export function Label({ children, style }: { children: React.ReactNode; style?: TextStyle }) {
  const t = useTheme();
  return <Text style={[{ fontSize: 12, fontWeight: '600', color: t.textSecondary },
                       style]}>{children}</Text>;
}

export function Body({ children, style, muted }: {
  children: React.ReactNode; style?: TextStyle; muted?: boolean;
}) {
  const t = useTheme();
  return <Text style={[{ fontSize: 14, color: muted ? t.textMuted : t.textPrimary },
                       style]}>{children}</Text>;
}

export function Button({ title, onPress, variant = 'default', disabled, loading }: {
  title: string;
  onPress: () => void;
  variant?: 'default' | 'primary' | 'danger';
  disabled?: boolean;
  loading?: boolean;
}) {
  const t = useTheme();
  const bg = variant === 'primary' ? t.series1
           : variant === 'danger'  ? t.critical
           : t.surfaceSunken;
  const fg = variant === 'default' ? t.textPrimary : '#ffffff';
  const isOff = disabled || loading;

  return (
    <Pressable
      onPress={onPress}
      disabled={isOff}
      // A charging control gets a generous hit area: it is pressed with cold
      // hands, in the rain, one-handed.
      hitSlop={8}
      style={({ pressed }) => ({
        backgroundColor: bg,
        opacity: isOff ? 0.5 : pressed ? 0.85 : 1,
        paddingVertical: 15,
        paddingHorizontal: spacing.xl,
        borderRadius: radius.md,
        borderWidth: variant === 'default' ? StyleSheet.hairlineWidth : 0,
        borderColor: t.border,
        alignItems: 'center',
        justifyContent: 'center',
        flexDirection: 'row',
        gap: spacing.sm,
        minHeight: 50,
      })}
      accessibilityRole="button"
      accessibilityState={{ disabled: isOff, busy: loading }}
    >
      {loading && <ActivityIndicator size="small" color={fg} />}
      <Text style={{ color: fg, fontSize: 16, fontWeight: '600' }}>{title}</Text>
    </Pressable>
  );
}

/** Status colour and glyph. Never colour alone — the word is always present. */
function statusStyle(t: Theme, status: ChargerStatus, online: boolean) {
  if (!online) return { color: t.idle, glyph: '⦸', text: 'Offline' };
  switch (status) {
    case 'Charging':      return { color: t.good,     glyph: '⚡', text: 'Charging' };
    case 'Available':     return { color: t.series1,  glyph: '○',  text: 'Available' };
    case 'Preparing':     return { color: t.warning,  glyph: '◐',  text: 'Preparing' };
    case 'Finishing':     return { color: t.warning,  glyph: '◑',  text: 'Finishing' };
    case 'SuspendedEV':   return { color: t.serious,  glyph: '‖',  text: 'Paused by car' };
    case 'SuspendedEVSE': return { color: t.serious,  glyph: '‖',  text: 'Paused' };
    case 'Reserved':      return { color: t.series2,  glyph: '◆',  text: 'Reserved' };
    case 'Faulted':       return { color: t.critical, glyph: '▲',  text: 'Faulted' };
    default:              return { color: t.idle,     glyph: '—',  text: 'Unavailable' };
  }
}

export function StatusPill({ status, online }: { status: ChargerStatus; online: boolean }) {
  const t = useTheme();
  const s = statusStyle(t, status, online);
  return (
    <View
      accessibilityLabel={`Status: ${s.text}`}
      style={{
        flexDirection: 'row', alignItems: 'center', gap: 6,
        backgroundColor: t.surfaceSunken,
        paddingVertical: 5, paddingHorizontal: 10,
        borderRadius: 999, alignSelf: 'flex-start',
        borderWidth: StyleSheet.hairlineWidth, borderColor: t.border,
      }}
    >
      <View style={{ width: 8, height: 8, borderRadius: 4, backgroundColor: s.color }} />
      <Text style={{ fontSize: 11, color: t.textSecondary }}>{s.glyph}</Text>
      <Text style={{ fontSize: 13, fontWeight: '600', color: t.textPrimary }}>{s.text}</Text>
    </View>
  );
}

/** A big number with a caption — the phone equivalent of a dashboard stat tile. */
export function Metric({ label, value, sub, tone }: {
  label: string; value: string; sub?: string; tone?: 'critical';
}) {
  const t = useTheme();
  return (
    <View style={{ flex: 1, minWidth: 120 }}>
      <Text style={{ fontSize: 12, color: t.textSecondary, marginBottom: 3 }}>{label}</Text>
      <Text style={{
        fontSize: 26, fontWeight: '700', letterSpacing: -0.5,
        color: tone === 'critical' ? t.critical : t.textPrimary,
      }}>{value}</Text>
      {sub ? <Text style={{ fontSize: 12, color: t.textMuted, marginTop: 2 }}>{sub}</Text> : null}
    </View>
  );
}

export function ErrorBanner({ message, onDismiss }: { message: string; onDismiss?: () => void }) {
  const t = useTheme();
  return (
    <Pressable onPress={onDismiss} accessibilityRole="alert">
      <View style={{
        backgroundColor: t.dark ? 'rgba(208,59,59,0.18)' : 'rgba(208,59,59,0.10)',
        borderColor: 'rgba(208,59,59,0.45)',
        borderWidth: StyleSheet.hairlineWidth,
        borderRadius: radius.sm,
        padding: spacing.md,
        marginBottom: spacing.md,
        flexDirection: 'row', alignItems: 'center', gap: spacing.sm,
      }}>
        <Text style={{ color: t.critical, fontSize: 14 }}>▲</Text>
        <Text style={{ color: t.textPrimary, fontSize: 13, flex: 1 }}>{message}</Text>
      </View>
    </Pressable>
  );
}

export function Empty({ text }: { text: string }) {
  const t = useTheme();
  return (
    <View style={{ padding: spacing.xxl, alignItems: 'center' }}>
      <Text style={{ color: t.textMuted, fontSize: 14, textAlign: 'center' }}>{text}</Text>
    </View>
  );
}

/**
 * A horizontal fill showing drawn current against the offered limit.
 *
 * A bar rather than a number because the interesting fact is the *ratio* — a
 * car drawing 16 A of an offered 16 A is fine, and drawing 16 A of an offered
 * 48 A means something is limiting it.
 */
export function CurrentMeter({ drawn, offered, max }: {
  drawn: number | null; offered: number | null; max: number;
}) {
  const t = useTheme();
  const ceiling = Math.max(offered ?? 0, max, 1);
  const drawnFrac = Math.min(1, Math.max(0, (drawn ?? 0) / ceiling));
  const offeredFrac = Math.min(1, Math.max(0, (offered ?? 0) / ceiling));

  return (
    <View
      accessibilityLabel={
        `Drawing ${drawn?.toFixed(1) ?? 'no'} amps of ${offered?.toFixed(0) ?? 'unknown'} offered`
      }
    >
      <View style={{
        height: 10, borderRadius: 5, backgroundColor: t.surfaceSunken, overflow: 'hidden',
      }}>
        {/* The offered limit is a track, the drawn current the fill inside it. */}
        <View style={{
          position: 'absolute', left: 0, top: 0, bottom: 0,
          width: `${offeredFrac * 100}%`,
          backgroundColor: t.dark ? 'rgba(255,255,255,0.14)' : 'rgba(11,11,11,0.10)',
        }} />
        <View style={{
          position: 'absolute', left: 0, top: 0, bottom: 0,
          width: `${drawnFrac * 100}%`,
          backgroundColor: t.series1, borderRadius: 5,
        }} />
      </View>
      <View style={{ flexDirection: 'row', justifyContent: 'space-between', marginTop: 5 }}>
        <Text style={{ fontSize: 11, color: t.textMuted }}>0 A</Text>
        <Text style={{ fontSize: 11, color: t.textSecondary }}>
          {drawn !== null ? `${drawn.toFixed(1)} A drawn` : 'not drawing'}
          {offered !== null ? ` · ${offered.toFixed(0)} A offered` : ''}
        </Text>
        <Text style={{ fontSize: 11, color: t.textMuted }}>{ceiling.toFixed(0)} A</Text>
      </View>
    </View>
  );
}
