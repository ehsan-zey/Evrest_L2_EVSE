/**
 * Design tokens, mirroring the dashboard's so the two products look related.
 *
 * A phone is used outdoors in direct sun and in a dark car park, often in the
 * same session, so both schemes have to be genuinely legible rather than one
 * being an afterthought.
 */
import { useColorScheme } from 'react-native';

export interface Theme {
  dark: boolean;
  surface: string;
  surfaceRaised: string;
  surfaceSunken: string;
  page: string;
  textPrimary: string;
  textSecondary: string;
  textMuted: string;
  border: string;
  series1: string;
  series2: string;
  good: string;
  warning: string;
  serious: string;
  critical: string;
  idle: string;
}

const light: Theme = {
  dark: false,
  surface: '#fcfcfb',
  surfaceRaised: '#ffffff',
  surfaceSunken: '#f2f2ee',
  page: '#f9f9f7',
  textPrimary: '#0b0b0b',
  textSecondary: '#52514e',
  textMuted: '#898781',
  border: 'rgba(11,11,11,0.10)',
  series1: '#2a78d6',
  series2: '#eb6834',
  good: '#0ca30c',
  warning: '#fab219',
  serious: '#ec835a',
  critical: '#d03b3b',
  idle: '#898781',
};

const dark: Theme = {
  dark: true,
  surface: '#1a1a19',
  surfaceRaised: '#232322',
  surfaceSunken: '#0d0d0d',
  page: '#0d0d0d',
  textPrimary: '#ffffff',
  textSecondary: '#c3c2b7',
  textMuted: '#898781',
  border: 'rgba(255,255,255,0.10)',
  series1: '#3987e5',
  series2: '#d95926',
  good: '#0ca30c',
  warning: '#fab219',
  serious: '#ec835a',
  critical: '#d03b3b',
  idle: '#898781',
};

export function useTheme(): Theme {
  return useColorScheme() === 'dark' ? dark : light;
}

export const spacing = { xs: 4, sm: 8, md: 12, lg: 16, xl: 24, xxl: 32 };
export const radius = { sm: 8, md: 12, lg: 16 };
