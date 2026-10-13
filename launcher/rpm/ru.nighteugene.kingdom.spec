Name: ru.nighteugene.kingdom
Summary: Kingdom: Two Crowns with bundled game data for Aurora OS
Version: 1.0.0
Release: 2
AutoReqProv: no
License: MIT and Proprietary
Source0: %{name}-%{version}.tar.bz2
BuildRequires: pkgconfig(Qt5Core)
BuildRequires: pkgconfig(Qt5Gui)
BuildRequires: pkgconfig(Qt5Qml)
BuildRequires: pkgconfig(Qt5Quick)
BuildRequires: pkgconfig(Qt5DBus)
BuildRequires: pkgconfig(auroraapp)
BuildRequires: pkgconfig(zlib)

%description
Standalone Kingdom: Two Crowns application for Aurora OS. Includes a compatible
game APK and native runtime; starts directly without an APK picker.

%prep
%autosetup

%build
%qmake5 CONFIG+=builtin
%make_build

%install
%make_install
mkdir -p %{buildroot}%{_datadir}/%{name}/runtime/lib/arm64-v8a
cp libandroid.so %{buildroot}%{_datadir}/%{name}/runtime/lib/arm64-v8a/
for library in libmain.so libunity.so libil2cpp.so lib_burst_generated.so libsteam_api.so; do
    cp ../builtin-source/runtime/lib/arm64-v8a/$library %{buildroot}%{_datadir}/%{name}/runtime/lib/arm64-v8a/
done
mkdir -p %{buildroot}%{_datadir}/%{name}/runtime/external %{buildroot}%{_datadir}/%{name}/runtime/files
cp -a ../builtin-source/runtime/external/il2cpp %{buildroot}%{_datadir}/%{name}/runtime/external/
cp -a ../builtin-source/runtime/files/il2cpp %{buildroot}%{_datadir}/%{name}/runtime/files/
mkdir -p %{buildroot}%{_datadir}/%{name}/game
cp ../builtin-source/game/base.apk %{buildroot}%{_datadir}/%{name}/game/

%files
%defattr(-,root,root,-)
%{_bindir}/%{name}
%defattr(644,root,root,-)
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.png
%{_datadir}/%{name}/runtime
%{_datadir}/%{name}/game
