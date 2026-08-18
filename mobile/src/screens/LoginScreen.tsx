import React, { useState, useEffect } from 'react';
import {
  View, Text, TextInput, StyleSheet, KeyboardAvoidingView,
  Platform, ScrollView, Pressable,
} from 'react-native';
import { api, setToken, getApiBase, setApiBase, ApiError, type User } from '../lib/api';
import { useTheme, spacing, radius } from '../lib/theme';
import { Button, ErrorBanner, Title } from '../components/ui';

export function LoginScreen({ onSignedIn }: { onSignedIn: (u: User) => void }) {
  const t = useTheme();
  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [base, setBase] = useState('');
  const [showServer, setShowServer] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  useEffect(() => { void getApiBase().then(setBase); }, []);

  async function submit(): Promise<void> {
    setBusy(true);
    setError(null);
    try {
      // Persist the server address before signing in, so a corrected address
      // takes effect on this attempt rather than the next one.
      if (base.trim()) await setApiBase(base.trim());
      const res = await api.post<{ token: string; user: User }>('/api/auth/login', {
        email: email.trim().toLowerCase(), password,
      });
      await setToken(res.token);
      onSignedIn(res.user);
    } catch (err) {
      setError(err instanceof ApiError ? err.message : 'Sign-in failed');
      // A connection failure is nearly always a wrong server address, so open
      // the field rather than making the driver hunt for it.
      if (err instanceof ApiError && err.status === 0) setShowServer(true);
    } finally {
      setBusy(false);
    }
  }

  const input = {
    backgroundColor: t.surface,
    borderColor: t.border,
    borderWidth: StyleSheet.hairlineWidth,
    borderRadius: radius.sm,
    paddingHorizontal: spacing.md,
    paddingVertical: 13,
    fontSize: 16,
    color: t.textPrimary,
    marginBottom: spacing.md,
  };

  return (
    <KeyboardAvoidingView
      style={{ flex: 1, backgroundColor: t.page }}
      behavior={Platform.OS === 'ios' ? 'padding' : undefined}
    >
      <ScrollView contentContainerStyle={{ flexGrow: 1, justifyContent: 'center', padding: spacing.xl }}>
        <Text style={{ fontSize: 32, fontWeight: '800', color: t.textPrimary, letterSpacing: -1 }}>
          EV<Text style={{ color: t.series1 }}>REST</Text>
        </Text>
        <Text style={{ color: t.textSecondary, marginTop: 4, marginBottom: spacing.xl }}>
          Sign in to start charging
        </Text>

        {error && <ErrorBanner message={error} onDismiss={() => setError(null)} />}

        <Text style={{ fontSize: 12, fontWeight: '600', color: t.textSecondary, marginBottom: 5 }}>
          Email
        </Text>
        <TextInput
          style={input}
          value={email}
          onChangeText={setEmail}
          autoCapitalize="none"
          autoCorrect={false}
          keyboardType="email-address"
          textContentType="username"
          placeholder="you@example.com"
          placeholderTextColor={t.textMuted}
        />

        <Text style={{ fontSize: 12, fontWeight: '600', color: t.textSecondary, marginBottom: 5 }}>
          Password
        </Text>
        <TextInput
          style={input}
          value={password}
          onChangeText={setPassword}
          secureTextEntry
          textContentType="password"
          placeholder="••••••••"
          placeholderTextColor={t.textMuted}
          onSubmitEditing={() => void submit()}
          returnKeyType="go"
        />

        <Button title="Sign in" variant="primary" loading={busy}
                disabled={!email.trim() || !password}
                onPress={() => void submit()} />

        <Pressable onPress={() => setShowServer((v) => !v)} hitSlop={10}
                   style={{ marginTop: spacing.xl, alignSelf: 'center' }}>
          <Text style={{ color: t.textMuted, fontSize: 13 }}>
            {showServer ? 'Hide server settings' : 'Server settings'}
          </Text>
        </Pressable>

        {showServer && (
          <View style={{ marginTop: spacing.md }}>
            <Text style={{ fontSize: 12, fontWeight: '600', color: t.textSecondary, marginBottom: 5 }}>
              CSMS address
            </Text>
            <TextInput
              style={input}
              value={base}
              onChangeText={setBase}
              autoCapitalize="none"
              autoCorrect={false}
              keyboardType="url"
              placeholder="http://192.168.1.10:9221"
              placeholderTextColor={t.textMuted}
            />
            <Text style={{ color: t.textMuted, fontSize: 12 }}>
              The API port of your Central System — not the OCPP port the chargers use.
            </Text>
          </View>
        )}
      </ScrollView>
    </KeyboardAvoidingView>
  );
}
