import React, { useEffect, useState, useCallback } from 'react';
import { View, ActivityIndicator, StatusBar as RNStatusBar, Text } from 'react-native';
import { NavigationContainer, DefaultTheme, DarkTheme } from '@react-navigation/native';
import { createNativeStackNavigator } from '@react-navigation/native-stack';
import { createBottomTabNavigator } from '@react-navigation/bottom-tabs';
import { SafeAreaProvider } from 'react-native-safe-area-context';
import { StatusBar } from 'expo-status-bar';

import { api, getToken, setToken, setUnauthorizedHandler, type User } from './src/lib/api';
import { useTheme } from './src/lib/theme';
import { LoginScreen } from './src/screens/LoginScreen';
import { ChargersScreen } from './src/screens/ChargersScreen';
import { ChargerDetailScreen } from './src/screens/ChargerDetailScreen';
import { HistoryScreen } from './src/screens/HistoryScreen';
import { ProfileScreen } from './src/screens/ProfileScreen';

const Stack = createNativeStackNavigator();
const Tabs = createBottomTabNavigator();

/**
 * Tab icons are text glyphs rather than an icon font.
 *
 * A vector icon package is another dependency, another asset pipeline and
 * another thing to keep in sync with the theme, for five icons. Glyphs render
 * identically on both platforms and inherit the tint colour for free.
 */
function TabIcon({ glyph, color }: { glyph: string; color: string }) {
  return <Text style={{ fontSize: 20, color }}>{glyph}</Text>;
}

function MainTabs({ user, onSignOut }: { user: User; onSignOut: () => void }) {
  const t = useTheme();
  return (
    <Tabs.Navigator
      screenOptions={{
        headerShown: false,
        tabBarActiveTintColor: t.series1,
        tabBarInactiveTintColor: t.textMuted,
        tabBarStyle: { backgroundColor: t.surface, borderTopColor: t.border },
      }}
    >
      <Tabs.Screen
        name="ChargersTab"
        options={{
          title: 'Chargers',
          tabBarIcon: ({ color }) => <TabIcon glyph="⚡" color={color} />,
        }}
      >
        {() => (
          <Stack.Navigator
            screenOptions={{
              headerStyle: { backgroundColor: t.surface },
              headerTintColor: t.textPrimary,
              headerShadowVisible: false,
            }}
          >
            <Stack.Screen name="Chargers" component={ChargersScreen}
                          options={{ headerShown: false }} />
            <Stack.Screen
              name="ChargerDetail"
              options={({ route }: any) => ({ title: route.params?.name ?? 'Charger' })}
            >
              {(props) => <ChargerDetailScreen {...props} user={user} />}
            </Stack.Screen>
          </Stack.Navigator>
        )}
      </Tabs.Screen>

      <Tabs.Screen
        name="History"
        component={HistoryScreen}
        options={{
          title: 'History',
          tabBarIcon: ({ color }) => <TabIcon glyph="◷" color={color} />,
        }}
      />

      <Tabs.Screen
        name="Profile"
        options={{
          title: 'Account',
          tabBarIcon: ({ color }) => <TabIcon glyph="◉" color={color} />,
        }}
      >
        {() => <ProfileScreen user={user} onSignOut={onSignOut} />}
      </Tabs.Screen>
    </Tabs.Navigator>
  );
}

export default function App() {
  const t = useTheme();
  const [user, setUser] = useState<User | null>(null);
  const [checking, setChecking] = useState(true);

  const signOut = useCallback(() => setUser(null), []);

  useEffect(() => {
    // A 401 anywhere in the app drops straight back to the login screen rather
    // than leaving a signed-out user staring at empty lists.
    setUnauthorizedHandler(signOut);
    return () => setUnauthorizedHandler(null);
  }, [signOut]);

  useEffect(() => {
    void (async () => {
      const token = await getToken();
      if (!token) { setChecking(false); return; }
      try {
        setUser(await api.get<User>('/api/auth/me'));
      } catch {
        // Stored token is stale or the server moved; start clean.
        await setToken(null);
      } finally {
        setChecking(false);
      }
    })();
  }, []);

  const navTheme = t.dark
    ? { ...DarkTheme, colors: { ...DarkTheme.colors, background: t.page, card: t.surface,
                                text: t.textPrimary, border: t.border, primary: t.series1 } }
    : { ...DefaultTheme, colors: { ...DefaultTheme.colors, background: t.page, card: t.surface,
                                   text: t.textPrimary, border: t.border, primary: t.series1 } };

  if (checking) {
    return (
      <View style={{ flex: 1, backgroundColor: t.page, alignItems: 'center',
                     justifyContent: 'center' }}>
        <ActivityIndicator color={t.series1} />
      </View>
    );
  }

  return (
    <SafeAreaProvider>
      <StatusBar style={t.dark ? 'light' : 'dark'} />
      <NavigationContainer theme={navTheme}>
        {user
          ? <MainTabs user={user} onSignOut={signOut} />
          : <LoginScreen onSignedIn={setUser} />}
      </NavigationContainer>
    </SafeAreaProvider>
  );
}
