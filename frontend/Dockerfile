# syntax=docker/dockerfile:1

# ---------------------------------------------------------------------------
# dev -- Vite dev server with hot reload over the bind-mounted sources.
# ---------------------------------------------------------------------------
FROM node:22-bookworm-slim AS dev

WORKDIR /app
ENV NODE_ENV=development

# Only the manifests are copied: the sources arrive through the compose bind
# mount, and node_modules lives in its own volume.
COPY package.json package-lock.json* ./
RUN npm install

EXPOSE 5173
CMD ["npm", "run", "dev"]

# ---------------------------------------------------------------------------
# builder -- typecheck, lint, test and produce the static bundle.
# ---------------------------------------------------------------------------
FROM node:22-bookworm-slim AS builder

WORKDIR /app
COPY package.json package-lock.json* ./
RUN npm ci || npm install
COPY . .
RUN npm run lint && npm run test && npm run build

# ---------------------------------------------------------------------------
# runtime -- static bundle behind nginx, with /api proxied to the hub.
# ---------------------------------------------------------------------------
FROM nginx:1.27-alpine AS runtime

COPY nginx.conf /etc/nginx/conf.d/default.conf
COPY --from=builder /app/dist /usr/share/nginx/html

EXPOSE 80
