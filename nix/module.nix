self:
{ config, lib, pkgs, ... }:

let
  cfg = config.services.globetrot;
in
{
  options.services.globetrot = {
    enable = lib.mkEnableOption "the Globetrot Globe/Fn key daemon";

    package = lib.mkOption {
      type = lib.types.package;
      default = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
      defaultText = lib.literalExpression "globetrot.packages.\${system}.default";
      description = "The globetrot package to use.";
    };

    settings = lib.mkOption {
      type = lib.types.lines;
      default = builtins.readFile ../config/globetrot.conf;
      defaultText = lib.literalMD "the shipped `config/globetrot.conf`";
      example = ''
        [globe]
        tap_mode = layout
        tap_timeout_ms = 300

        [bindings]
        globe+e = action:emoji
      '';
      description = "Contents of /etc/globetrot.conf.";
    };

    sessionAgent = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = ''
        Start globetrot-session in graphical sessions. It performs the layout
        switch (KDE, Hyprland, niri, Sway, GNOME, fcitx5) and runs Globe actions.
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    boot.kernelModules = [ "uinput" ];

    environment.systemPackages = [ cfg.package ];
    systemd.packages = [ cfg.package ];

    environment.etc."globetrot.conf".text = cfg.settings;

    # libinput on NixOS reads only its own quirks plus this override file. Marking the
    # virtual keyboard internal keeps disable-while-typing for the internal touchpad.
    environment.etc."libinput/local-overrides.quirks".text =
      builtins.readFile ../libinput/60-globetrot.quirks;

    systemd.services.globetrotd = {
      wantedBy = [ "multi-user.target" ];
      restartTriggers = [ cfg.settings ];
    };

    systemd.user.services.globetrot-session = lib.mkIf cfg.sessionAgent {
      wantedBy = [ "graphical-session.target" ];
    };
  };
}
