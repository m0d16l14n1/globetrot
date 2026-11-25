{ lib, stdenv, python3, kmod, coreutils }:

stdenv.mkDerivation {
  pname = "globetrot";
  version = "0.4.0";

  src = lib.cleanSourceWith {
    src = ../.;
    filter = path: type:
      let base = baseNameOf path; in
      !(builtins.elem base [ "build" ".git" "result" "__pycache__" ]);
  };

  nativeBuildInputs = [ python3 ];
  buildInputs = [ python3 ];

  buildFlags = [ "daemon" ];

  doCheck = true;
  checkTarget = "test";
  # LeakSanitizer needs ptrace, which the build sandbox may not allow.
  preCheck = ''
    export ASAN_OPTIONS=detect_leaks=0
  '';

  installFlags = [
    "PREFIX=${placeholder "out"}"
    "SYSCONFDIR=/etc"
    "ETCDIR=${placeholder "out"}/share/globetrot"
    "MODPROBE=${kmod}/bin/modprobe"
    "KILL=${coreutils}/bin/kill"
  ];

  meta = {
    description = "Globe/Fn key for Linux: tap to switch keyboard layout, hold for the Fn layer and shortcuts";
    homepage = "https://github.com/m0d16l14n1/globetrot";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "globetrot";
  };
}
