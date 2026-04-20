%global moddir   /usr/lib/valkey/modules
%global confdir  /etc/valkey/modules-available

Name:           valkey-fractalsql
Version:        1.0.0
Release:        1%{?dist}
Summary:        Stochastic Fractal Search module for Valkey

License:        MIT
URL:            https://github.com/FractalSQLabs/valkey-fractalsql
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc, make, curl, git
Requires:       valkey

BuildArch:      %{_arch}

%description
valkey-fractalsql is a native C module for Valkey 7.2 / 8.0 / 8.1
that exposes a FRACTAL.SEARCH command backed by a LuaJIT-compiled
Stochastic Fractal Search optimizer. The module ABI
(REDISMODULE_APIVER_1, aliased as VALKEYMODULE_APIVER_1) was
preserved across the April 2024 fork from Redis, so one .so per
arch covers every currently-shipping Valkey major.

The command is declared readonly / fast / allow-stale so the Valkey
8.x scheduler can dispatch it to a worker thread. LuaJIT is
statically linked; the only runtime dep is glibc.

Enable by adding to valkey.conf:

    loadmodule /usr/lib/valkey/modules/fractalsql.so

An example config snippet is installed at
/etc/valkey/modules-available/fractalsql.conf.

%prep
%setup -q

%build
# The per-arch .so is produced out-of-band by build.sh on a Docker
# builder; this spec stages the prebuilt artifact.
test -f dist/%{_arch}/fractalsql.so

%install
# Claim %dir ownership of the Valkey modules directory since no base
# package owns it on stock RHEL-family installs.
install -d -m 0755 %{buildroot}%{moddir}
install -d -m 0755 %{buildroot}%{confdir}

install -Dm0755 dist/%{_arch}/fractalsql.so \
    %{buildroot}%{moddir}/fractalsql.so
install -Dm0644 scripts/load_module.conf \
    %{buildroot}%{confdir}/fractalsql.conf
install -Dm0644 LICENSE \
    %{buildroot}%{_docdir}/%{name}/LICENSE
install -Dm0644 LICENSE-THIRD-PARTY \
    %{buildroot}%{_docdir}/%{name}/LICENSE-THIRD-PARTY

%files
%license LICENSE
%dir %{moddir}
%dir %{confdir}
%{moddir}/fractalsql.so
%{_docdir}/%{name}/LICENSE
%{_docdir}/%{name}/LICENSE-THIRD-PARTY
%config(noreplace) %{confdir}/fractalsql.conf

%post
cat <<'EOF'

valkey-fractalsql installed.

The module is at:
    /usr/lib/valkey/modules/fractalsql.so

Enable it by adding this to valkey.conf:

    loadmodule /usr/lib/valkey/modules/fractalsql.so

Or include the shipped snippet:

    include /etc/valkey/modules-available/fractalsql.conf

Then restart the server:

    sudo systemctl restart valkey

Verify:

    valkey-cli MODULE LIST
    valkey-cli FRACTALSQL.EDITION       # -> "Community"
    valkey-cli FRACTALSQL.VERSION       # -> "1.0.0"
    valkey-cli COMMAND INFO FRACTAL.SEARCH

EOF

%changelog
* Sat Apr 18 2026 FractalSQLabs <ops@fractalsqlabs.io> - 1.0.0-1
- Initial Factory-standardized release for Valkey 7.2 / 8.0 / 8.1.
  Static LuaJIT, zero-dependency posture. One .so per arch covers
  every currently-shipping Valkey major. Verified on AMD64 and ARM64.
