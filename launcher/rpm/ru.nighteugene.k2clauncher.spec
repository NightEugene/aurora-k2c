Name: ru.nighteugene.k2clauncher
Summary: APK launcher for Kingdom: Two Crowns on Aurora OS
Version: 1.1.1
Release: 1
AutoReqProv: no
License: MIT
Source0: %{name}-%{version}.tar.bz2
BuildRequires: pkgconfig(Qt5Core)
BuildRequires: pkgconfig(Qt5Gui)
BuildRequires: pkgconfig(Qt5Qml)
BuildRequires: pkgconfig(Qt5Quick)
BuildRequires: pkgconfig(Qt5DBus)
BuildRequires: pkgconfig(auroraapp)
BuildRequires: pkgconfig(zlib)

%description
Aurora OS application that selects a compatible game APK and starts the prepared native game runtime.

%prep
%autosetup

%build
%qmake5
%make_build

%install
%make_install
mkdir -p %{buildroot}%{_datadir}/%{name}/runtime
mkdir -p %{buildroot}%{_datadir}/%{name}/runtime/lib/arm64-v8a
cp libandroid.so %{buildroot}%{_datadir}/%{name}/runtime/lib/arm64-v8a/
for library in libmain.so libunity.so libil2cpp.so lib_burst_generated.so libsteam_api.so libPDALIFE.so libpdaprotect.so; do
    cp ../rpm-source/runtime/lib/arm64-v8a/$library %{buildroot}%{_datadir}/%{name}/runtime/lib/arm64-v8a/
done
mkdir -p %{buildroot}%{_datadir}/%{name}/runtime/external %{buildroot}%{_datadir}/%{name}/runtime/files
cp -a ../rpm-source/runtime/external/il2cpp %{buildroot}%{_datadir}/%{name}/runtime/external/
cp -a ../rpm-source/runtime/files/il2cpp %{buildroot}%{_datadir}/%{name}/runtime/files/

%files
%defattr(-,root,root,-)
%{_bindir}/%{name}
%defattr(644,root,root,-)
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.png
%{_datadir}/%{name}/runtime
