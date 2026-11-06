import { useEffect, useState } from 'react';

import { api, ApiError, clearToken, getToken } from '@/api/client';
import { Dashboard } from '@/components/Dashboard';
import { LoginForm } from '@/components/LoginForm';
import type { User } from '@/types';

export function App() {
  const [user, setUser] = useState<User | null>(null);
  const [checking, setChecking] = useState(getToken() !== null);
  const [error, setError] = useState<string | null>(null);
  const [attempt, setAttempt] = useState(0);
  const [signingOut, setSigningOut] = useState(false);

  useEffect(() => {
    let cancelled = false;
    const onUnauthorized = (): void => {
      cancelled = true;
      setUser(null);
      setChecking(false);
      setError(null);
    };
    window.addEventListener('homestr:unauthorized', onUnauthorized);
    if (getToken() !== null) {
      api
        .me()
        .then((currentUser) => {
          if (!cancelled) {
            setUser(currentUser);
            setChecking(false);
          }
        })
        .catch((cause: unknown) => {
          if (cancelled) {
            return;
          }
          setChecking(false);
          if (cause instanceof ApiError && cause.status === 401) {
            clearToken();
            setUser(null);
          } else {
            setError('Could not verify your session. Check your connection and retry.');
          }
        });
    }
    return () => {
      cancelled = true;
      window.removeEventListener('homestr:unauthorized', onUnauthorized);
    };
  }, [attempt]);

  const signOut = async (): Promise<void> => {
    setSigningOut(true);
    setError(null);
    try {
      await api.logout();
      setUser(null);
    } catch (cause) {
      if (!(cause instanceof ApiError && cause.status === 401)) {
        setError('Could not sign out. Please retry.');
      }
    } finally {
      setSigningOut(false);
    }
  };

  if (checking) {
    return <p role="status">Checking your session…</p>;
  }

  if (user === null) {
    if (error !== null) {
      return (
        <main>
          <p role="alert">{error}</p>
          <button
            type="button"
            onClick={() => {
              setError(null);
              setChecking(true);
              setAttempt((current) => current + 1);
            }}
          >
            Retry
          </button>
        </main>
      );
    }
    return <LoginForm onAuthenticated={setUser} />;
  }

  return (
    <>
      <header className="app-bar">
        <strong>HomeStr</strong>
        <span>{user.email}</span>
        <button type="button" disabled={signingOut} onClick={() => void signOut()}>
          {signingOut ? 'Signing out…' : 'Sign out'}
        </button>
      </header>
      {error !== null && <p role="alert">{error}</p>}
      <Dashboard user={user} />
    </>
  );
}
