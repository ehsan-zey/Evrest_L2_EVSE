import React, { useState, useEffect } from 'react';
import { View, Text, ScrollView, StyleSheet, TextInput } from 'react-native';
import { api, setToken, getApiBase, setApiBase, type User, type Tag } from '../lib/api';
import { useTheme, spacing, radius } from '../lib/theme';
import { Card, Title, Body, Label, Button, Empty } from '../components/ui';

export function ProfileScreen({ user, onSignOut }: { user: User; onSignOut: () => void }) {
  const t = useTheme();
  const [tags, setTags] = useState<Tag[]>([]);
  const [base, setBase] = useState('');
  const [saved, setSaved] = useState(false);

  useEffect(() => {
    void api.get<Tag[]>('/api/tags').then(setTags).catch(() => { /* non-fatal */ });
    void getApiBase().then(setBase);
  }, []);

  return (
    <ScrollView style={{ flex: 1, backgroundColor: t.page }}
                contentContainerStyle={{ padding: spacing.lg, paddingBottom: spacing.xxl * 2 }}>
      <Title style={{ marginBottom: spacing.lg }}>Account</Title>

      <Card style={{ marginBottom: spacing.md }}>
        <Text style={{ fontSize: 17, fontWeight: '600', color: t.textPrimary }}>
          {user.name ?? user.email}
        </Text>
        <Body muted style={{ marginTop: 2 }}>{user.email}</Body>
        <View style={{
          marginTop: spacing.md, alignSelf: 'flex-start',
          backgroundColor: t.surfaceSunken, paddingHorizontal: 10, paddingVertical: 4,
          borderRadius: 999,
        }}>
          <Text style={{ fontSize: 12, color: t.textSecondary, textTransform: 'capitalize' }}>
            {user.role}
          </Text>
        </View>
      </Card>

      <Card style={{ marginBottom: spacing.md }}>
        <Label style={{ marginBottom: spacing.md }}>YOUR CHARGING TAGS</Label>
        {tags.length === 0 ? (
          <Body muted>
            No tags assigned. A tag is what authorises a session — ask your operator to add one.
          </Body>
        ) : (
          tags.map((tag, i) => (
            <View key={tag.id_tag} style={{
              flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center',
              paddingVertical: spacing.md,
              borderBottomWidth: i === tags.length - 1 ? 0 : StyleSheet.hairlineWidth,
              borderBottomColor: t.border,
            }}>
              <View>
                <Text style={{ fontSize: 14, color: t.textPrimary, fontWeight: '500' }}>
                  {tag.label ?? tag.id_tag}
                </Text>
                <Text style={{ fontSize: 12, color: t.textMuted, marginTop: 2 }}>
                  {tag.id_tag}
                </Text>
              </View>
              <Text style={{
                fontSize: 13, fontWeight: '600',
                color: tag.status === 'Accepted' ? t.good : t.critical,
              }}>
                {tag.status === 'Accepted' ? '✓ Active' : `✕ ${tag.status}`}
              </Text>
            </View>
          ))
        )}
      </Card>

      <Card style={{ marginBottom: spacing.md }}>
        <Label style={{ marginBottom: spacing.md }}>SERVER</Label>
        <TextInput
          value={base}
          onChangeText={(v) => { setBase(v); setSaved(false); }}
          autoCapitalize="none"
          autoCorrect={false}
          keyboardType="url"
          style={{
            backgroundColor: t.surfaceSunken,
            borderRadius: radius.sm,
            paddingHorizontal: spacing.md, paddingVertical: 12,
            fontSize: 15, color: t.textPrimary,
            borderWidth: StyleSheet.hairlineWidth, borderColor: t.border,
          }}
        />
        <View style={{ marginTop: spacing.md }}>
          <Button
            title="Save and sign out"
            disabled={base.trim() === '' || saved}
            onPress={() => {
              /*
               * Saving a new address signs the user out. A token issued by one
               * CSMS is meaningless to another, so staying signed in would give
               * a confusing 401 on the next request instead of a login screen.
               */
              setSaved(true);
              void setApiBase(base.trim())
                .then(() => setToken(null))
                .then(onSignOut);
            }}
          />
        </View>
        <Body muted style={{ marginTop: spacing.md, fontSize: 12 }}>
          Changing this signs you out, since a token from one server is not valid on another.
        </Body>
      </Card>

      <Button title="Sign out" variant="danger"
              onPress={() => { void setToken(null).then(onSignOut); }} />
    </ScrollView>
  );
}
