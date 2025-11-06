# Docker-based DVS Testing

This guide explains how to run DVS (Docker Virtual Switch) tests using Docker Compose, without installing packages on your host machine.

## Architecture

The test environment consists of 3 containers:

1. **sw** (debian:bookworm) - Provides network namespace for VirtualServers (emulated hosts/peers)
2. **sonic-vs** (docker-sonic-vs) - The Device Under Test (DUT) - SONiC virtual switch
3. **test-runner** - Runs pytest with all test dependencies installed

## Prerequisites

### 1. Host Requirements

Load the `team` kernel module on the host (required for PortChannel/LAG functionality):

```bash
sudo modprobe team

# Verify it's loaded
lsmod | grep team
```

**Optional:** Make it persistent across reboots:

```bash
echo "team" | sudo tee -a /etc/modules
```

### 2. Docker and Docker Compose

- [Install Docker CE](https://docs.docker.com/install/linux/docker-ce/ubuntu/)
- Follow [post-install instructions](https://docs.docker.com/install/linux/linux-postinstall/) to run docker without sudo

### 3. Get docker-sonic-vs Image

**Option A: Download from Azure (Recommended)**

```bash
wget "https://sonic-build.azurewebsites.net/api/sonic/artifacts?branchName=master&platform=vs&target=target/docker-sonic-vs.gz" \
  -O docker-sonic-vs.gz
docker load < docker-sonic-vs.gz
```

**Option B: Build from sonic-buildimage**

Follow the [sonic-buildimage](https://github.com/sonic-net/sonic-buildimage) build instructions to build `docker-sonic-vs`.

### 4. Get sonic-buildimage Debian Packages

You need the `libswsscommon` and `python3-swsscommon` packages from sonic-buildimage.

**Option A: Download from Azure**

```bash
# Download from sonic-buildimage artifacts
# Set the path to where you downloaded/extracted the debs
export SONIC_BUILDIMAGE_DEBS=/path/to/sonic-buildimage/target/debs/bookworm
```

**Option B: Build from sonic-buildimage**

```bash
# After building sonic-buildimage
export SONIC_BUILDIMAGE_DEBS=/path/to/sonic-buildimage/target/debs/bookworm
```

## Quick Start

### 1. Build the test-runner image

```bash
docker-compose -f docker-compose.test.yml build test-runner
```

### 2. Run all tests

```bash
# Make sure team module is loaded
sudo modprobe team

# Run tests
SONIC_BUILDIMAGE_DEBS=/path/to/debs \
  docker-compose -f docker-compose.test.yml up --abort-on-container-exit test-runner
```

### 3. Run specific tests

```bash
# Override the default command to run specific tests
SONIC_BUILDIMAGE_DEBS=/path/to/debs \
  docker-compose -f docker-compose.test.yml run --rm test-runner \
  pytest -v --dvsname=sonic-vs test_portchannel.py
```

## Advanced Usage

### Run tests interactively

```bash
# Start containers
SONIC_BUILDIMAGE_DEBS=/path/to/debs \
  docker-compose -f docker-compose.test.yml up -d sw sonic-vs

# Build and run test-runner interactively
SONIC_BUILDIMAGE_DEBS=/path/to/debs \
  docker-compose -f docker-compose.test.yml run --rm test-runner bash

# Inside the container, you can run tests manually:
pytest -v --dvsname=sonic-vs test_vlan.py
pytest -v --dvsname=sonic-vs test_neighbor.py::TestNeighbor::test_Neighbor
```

### Rebuild test-runner image

If you modify the Dockerfile or need to update dependencies:

```bash
docker-compose -f docker-compose.test.yml build --no-cache test-runner
```

### Clean up

```bash
# Stop and remove all containers
docker-compose -f docker-compose.test.yml down

# Remove volumes
docker-compose -f docker-compose.test.yml down -v

# Remove test-runner image
docker rmi sonic-swss-test-runner:latest
```

### View logs

```bash
# View logs from all containers
docker-compose -f docker-compose.test.yml logs

# View logs from specific container
docker-compose -f docker-compose.test.yml logs sonic-vs
docker-compose -f docker-compose.test.yml logs test-runner

# Follow logs in real-time
docker-compose -f docker-compose.test.yml logs -f test-runner
```

## Troubleshooting

### Error: "team kernel module is not loaded"

**Solution:** Load the team module on the host:

```bash
sudo modprobe team
lsmod | grep team
```

### Error: "Cannot connect to the Docker daemon"

**Solution:** Make sure Docker is running and you have permissions:

```bash
sudo systemctl start docker
sudo usermod -aG docker $USER
# Log out and log back in for group changes to take effect
```

### Error: "/debs directory not found"

**Solution:** Make sure you set the `SONIC_BUILDIMAGE_DEBS` environment variable:

```bash
export SONIC_BUILDIMAGE_DEBS=/path/to/sonic-buildimage/target/debs/bookworm
```

### Error: "docker-sonic-vs:latest not found"

**Solution:** Download or build the docker-sonic-vs image (see Prerequisites section).

### Tests fail with "Connection refused" to Redis

**Solution:** Make sure the sonic-vs container is running and healthy:

```bash
docker-compose -f docker-compose.test.yml ps
docker-compose -f docker-compose.test.yml logs sonic-vs
```

### Permission denied errors

**Solution:** The test-runner needs privileged mode and access to host resources. Make sure:
- The test-runner service has `privileged: true`
- You're running docker-compose with appropriate permissions

## Differences from Host-based Testing

| Aspect | Host-based (tests/README.md) | Docker-based (this guide) |
|--------|------------------------------|---------------------------|
| **Package installation** | On host | In container |
| **Isolation** | Low | High |
| **Cleanup** | Manual | Automatic |
| **Reproducibility** | Depends on host | Consistent |
| **Setup time** | Fast (after initial setup) | Slower (first build) |
| **Host modifications** | Many packages | Only `team` module |

## Architecture Details

### Container Communication

- **sw ↔ sonic-vs**: `sonic-vs` uses `network_mode: "container:sonic-test-sw"` to share network namespace
- **test-runner ↔ sonic-vs**: Via Redis Unix socket at `/var/run/redis/redis.sock` (shared volume)
- **test-runner ↔ sw**: Via `nsenter` to enter sw's network namespace (requires `pid: "host"`)

### Volume Mounts

- `.:/workspace` - Mount sonic-swss source code
- `redis-socket:/var/run/redis` - Share Redis socket between containers
- `/var/run/docker.sock` - Allow test-runner to manage Docker containers
- `/var/run/netns` - Access network namespaces
- `${SONIC_BUILDIMAGE_DEBS}:/debs:ro` - Mount sonic-buildimage debs (read-only)

### Why Privileged Mode?

The containers need privileged mode for:
- Creating network namespaces (`ip netns add`)
- Creating veth pairs (`ip link add`)
- Loading kernel modules (checking `team` module)
- Accessing host network namespaces (`nsenter`)

## Contributing

If you find issues or have improvements, please update this documentation and the related files:
- `tests/Dockerfile.test-runner` - Test runner container image
- `docker-compose.test.yml` - Docker Compose configuration
- `tests/DOCKER_TESTING.md` - This documentation

