<template>
  <a-menu mode="horizontal" :style="{ lineHeight: '30px', borderBottom: 'none' }" @click="handleMenuClick">
    <a-sub-menu key="file" title="File">
      <a-menu-item key="file:open">Open Configuration...</a-menu-item>
      <a-menu-item key="file:save">Save Configuration</a-menu-item>
      <a-menu-divider />
      <a-menu-item key="file:exit">Exit</a-menu-item>
    </a-sub-menu>
    <a-sub-menu key="edit" title="Edit">
      <a-menu-item key="edit:undo">Undo</a-menu-item>
      <a-menu-item key="edit:redo">Redo</a-menu-item>
      <a-menu-divider />
      <a-menu-item key="edit:cut">Cut</a-menu-item>
      <a-menu-item key="edit:copy">Copy</a-menu-item>
      <a-menu-item key="edit:paste">Paste</a-menu-item>
    </a-sub-menu>
    <a-sub-menu key="view" title="View">
      <a-menu-item key="view:toggle-terminal">Toggle Terminal</a-menu-item>
    </a-sub-menu>
    <a-sub-menu key="help" title="Help">
      <a-menu-item key="help:about">About ESS</a-menu-item>
    </a-sub-menu>
  </a-menu>
</template>

<script setup>
const emit = defineEmits(['toggle-terminal'])

function handleMenuClick(e) {
  if (e.key === 'view:toggle-terminal') {
    emit('toggle-terminal')
  }
  // Future menu item handlers can be added here
}
</script>
