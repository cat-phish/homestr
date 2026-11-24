#!/bin/sh
set -e

# Substitute environment variables in the nginx config
envsubst '${BACKEND_HOST},${BACKEND_PORT}' < /etc/nginx/conf.d/default.conf.template > /etc/nginx/conf.d/default.conf

# Start nginx
exec nginx -g 'daemon off;'
