import { useState } from 'react';

import { api, ApiError } from '@/api/client';
import type { User } from '@/types';

interface LoginFormProps {
  onAuthenticated: (user: User) => void;
}

export function LoginForm({ onAuthenticated }: LoginFormProps) {
  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [pending, setPending] = useState(false);

  const handleSubmit = async (event: React.FormEvent<HTMLFormElement>): Promise<void> => {
    event.preventDefault();
    setPending(true);
    setError(null);
    try {
      const { user } = await api.login(email, password);
      onAuthenticated(user);
    } catch (cause) {
      setError(cause instanceof ApiError ? cause.message : 'Could not reach the hub');
    } finally {
      setPending(false);
    }
  };

  return (
    <form className="login" onSubmit={(event) => void handleSubmit(event)}>
      <h1>HomeStr</h1>

      <label htmlFor="email">Email</label>
      <input
        id="email"
        name="email"
        type="email"
        autoComplete="username"
        required
        value={email}
        onChange={(event) => setEmail(event.target.value)}
      />

      <label htmlFor="password">Password</label>
      <input
        id="password"
        name="password"
        type="password"
        autoComplete="current-password"
        required
        value={password}
        onChange={(event) => setPassword(event.target.value)}
      />

      {error !== null && <p role="alert">{error}</p>}

      <button type="submit" disabled={pending}>
        {pending ? 'Signing in…' : 'Sign in'}
      </button>
    </form>
  );
}
