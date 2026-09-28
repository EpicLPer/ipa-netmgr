ipa_netmgr_sources = [
  'main.c',
  'config.c',
  'log.c',
  'dpm.c',
  'wda.c',
  'call.c',
  'link.c',
  'modem.c',
  'raw.c',
  'at.c',
]

executable('ipa-netmgr',
  ipa_netmgr_sources,
  dependencies: [glib_dep, gobject_dep, gio_dep, qmi_dep, qrtr_dep],
  install: true)
