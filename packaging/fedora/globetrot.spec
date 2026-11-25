Name:           globetrot
Version:        0.4.0
Release:        1%{?dist}
Summary:        Globe/Fn key for Linux: tap to switch keyboard layout
License:        MIT
URL:            https://github.com/m0d16l14n1/globetrot
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  python3
BuildRequires:  systemd-rpm-macros
# The test suite runs under AddressSanitizer and UndefinedBehaviorSanitizer.
BuildRequires:  libasan
BuildRequires:  libubsan
Requires:       python3
Requires:       systemd

%description
Makes the Globe (fn) key on Apple keyboards behave like on macOS: a quick tap
switches the keyboard layout, holding it keeps the Fn layer (Delete, Home/End,
media keys) and allows Globe+key shortcuts. Works with KDE Plasma, Hyprland,
niri, Sway, GNOME and fcitx5.

%prep
%autosetup

%build
%make_build daemon CFLAGS="%{optflags}" LDFLAGS="%{build_ldflags}"

%check
make test

%install
%make_install PREFIX=%{_prefix} \
    UNITDIR=%{_unitdir} USERUNITDIR=%{_userunitdir} \
    MODULESLOADDIR=%{_modulesloaddir} SYSCONFDIR=%{_sysconfdir} \
    DOCDIR=%{_docdir}/%{name} LICENSEDIR=%{_licensedir}/%{name} \
    MODPROBE=/usr/sbin/modprobe KILL=/usr/bin/kill

%post
%systemd_post globetrotd.service
%systemd_user_post globetrot-session.service

%preun
%systemd_preun globetrotd.service
%systemd_user_preun globetrot-session.service

%postun
%systemd_postun_with_restart globetrotd.service
%systemd_user_postun globetrot-session.service

%files
%license %{_licensedir}/%{name}/LICENSE
%{_docdir}/%{name}/
%config(noreplace) %{_sysconfdir}/globetrot.conf
%{_bindir}/globetrotd
%{_bindir}/globetrot
%{_bindir}/globetrot-session
%{_prefix}/lib/globetrot/
%{_unitdir}/globetrotd.service
%{_userunitdir}/globetrot-session.service
%{_modulesloaddir}/globetrot.conf
%{_datadir}/libinput/60-globetrot.quirks

%changelog
* Wed Nov 25 2026 Nick <nekreshen@yandex.ru> - 0.4.0-1
- First standalone release
