Summary: Test VT100-type terminal
%global patchdate 20251205
# $XTermId: vttest.spec,v 1.78 2025/12/05 23:10:17 tom Exp $
Name: vttest
Version: 2.7.%{patchdate}
Release: 1
License: MIT
Group: Applications/Development
URL: https://invisible-island.net/%{name}/
Source0: https://invisible-island.net/archives/%{name}/%{name}-%{patchdate}.tgz

%description
Vttest is a program designed to test the functionality of a VT100
terminal (or emulator thereof).  It tests both display (escape sequence
handling) and keyboard.

The program is menu-driven and contains full on-line operating
instructions.  To run a given menu-item, you must enter its number.  You
can run all menu-items (for a given level) by entering an asterisk, i.e,
`*'.

%prep

# no need for debugging symbols...
%define debug_package %{nil}

%setup -q -n %{name}-%{patchdate}

%build

INSTALL_PROGRAM='${INSTALL}' \
%configure \
  --target %{_target_platform} \
  --prefix=%{_prefix} \
  --bindir=%{_bindir} \
  --libdir=%{_libdir} \
  --mandir=%{_mandir}

make

%install
[ "$RPM_BUILD_ROOT" != "/" ] && rm -rf $RPM_BUILD_ROOT

make install                    DESTDIR=$RPM_BUILD_ROOT

strip $RPM_BUILD_ROOT%{_bindir}/%{name}

%files
%defattr(-,root,root)
%{_prefix}/bin/%{name}
%{_mandir}/man1/%{name}.*

%changelog
# each patch should add its ChangeLog entries here

* Fri Dec 05 2025 Thomas E. Dickey
- testing vttest 2.7.20251205-1

* Thu Dec 29 2022 Thomas E. Dickey
- update URLs

* Mon Jul 02 2018 Thomas E. Dickey
- use recommended flags for build

* Thu Aug 26 2010 Thomas E. Dickey
- initial version
