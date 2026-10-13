import { useState } from 'react';

import { api, getToken } from '@/api/client';
import { Dashboard } from '@/components/Dashboard';
import { LoginForm } from '@/components/LoginForm';
import type { User } from '@/types';

export function App() {
  // A stored token survives a reload; the user object is refetched on login.
  const [user, setUser] = useState<User | null>(null);
  const authenticated = user !== null || getToken() !== null;

  if (!authenticated) {
    return <LoginForm onAuthenticated={setUser} />;
  }

  return (
    <>
      <header className="app-bar">
        <strong>HomeStr</strong>
        {user !== null && <span>{user.email}</span>}
        <button
          type="button"
          onClick={() => {
            api.logout();
            setUser(null);
          }}
        >
          Sign out
        </button>
      </header>
      <Dashboard />
    </>
  );
}
