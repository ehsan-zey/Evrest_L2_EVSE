/**
 * Session tokens for the dashboard and the mobile app.
 *
 * Chargers do not use these — they authenticate with HTTP Basic over TLS at the
 * WebSocket handshake. Keeping the two credential systems separate means a
 * leaked dashboard token cannot impersonate a charger, and vice versa.
 */
import jwt from 'jsonwebtoken';
import type { Request, Response, NextFunction } from 'express';

export type Role = 'admin' | 'operator' | 'driver';

export interface TokenPayload {
  sub: string;      // user id
  email: string;
  role: Role;
}

/** Long enough not to annoy an operator mid-shift, short enough to matter. */
const TOKEN_TTL = '12h';

export function signToken(secret: string, payload: TokenPayload): string {
  return jwt.sign(payload, secret, { expiresIn: TOKEN_TTL });
}

export function verifyToken(secret: string, token: string): TokenPayload | null {
  try {
    return jwt.verify(token, secret) as TokenPayload;
  } catch {
    return null;
  }
}

declare global {
  // eslint-disable-next-line @typescript-eslint/no-namespace
  namespace Express {
    interface Request { user?: TokenPayload }
  }
}

/** Require a valid token; attaches `req.user`. */
export function requireAuth(secret: string) {
  return (req: Request, res: Response, next: NextFunction): void => {
    const header = req.headers.authorization;
    if (!header?.startsWith('Bearer ')) {
      res.status(401).json({ error: 'Missing bearer token' });
      return;
    }
    const payload = verifyToken(secret, header.slice(7));
    if (!payload) {
      res.status(401).json({ error: 'Invalid or expired token' });
      return;
    }
    req.user = payload;
    next();
  };
}

/**
 * Require one of @p roles. Ordered so `admin` implies everything below it —
 * an operator endpoint should not need to list admin explicitly and then be
 * forgotten when a new one is added.
 */
const RANK: Record<Role, number> = { driver: 0, operator: 1, admin: 2 };

export function requireRole(min: Role) {
  return (req: Request, res: Response, next: NextFunction): void => {
    if (!req.user) {
      res.status(401).json({ error: 'Not authenticated' });
      return;
    }
    if (RANK[req.user.role] < RANK[min]) {
      res.status(403).json({ error: `Requires ${min} role` });
      return;
    }
    next();
  };
}
