#!/bin/bash
set -e

# Stop any running server
su - postgres -c '/usr/local/pgsql/bin/pg_ctl -D /home/postgres/data stop -m fast 2>/dev/null' || true

# Reinitialize database
rm -rf /home/postgres/data
mkdir -p /home/postgres
chown postgres:postgres /home/postgres
su - postgres -c '/usr/local/pgsql/bin/initdb -D /home/postgres/data -U postgres --auth=trust'

# Set port to 5434 to avoid conflict
cat >> /home/postgres/data/postgresql.conf << 'EOF'
port = 5434
listen_addresses = '*'
EOF
chown postgres:postgres /home/postgres/data/postgresql.conf

# Start server
su - postgres -c '/usr/local/pgsql/bin/pg_ctl -D /home/postgres/data -l /home/postgres/logfile start'

# Wait and install extensions
sleep 2
su - postgres -c '/usr/local/pgsql/bin/psql -p 5434 -c "CREATE EXTENSION IF NOT EXISTS vector"'
su - postgres -c '/usr/local/pgsql/bin/psql -p 5434 -c "CREATE EXTENSION IF NOT EXISTS jolix_predict"'

echo "=== Server started on port 5434 ==="
