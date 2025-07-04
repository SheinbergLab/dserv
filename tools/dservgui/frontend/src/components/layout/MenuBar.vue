<template>
  <div class="menu-bar">
    <div 
      v-for="menu in menuItems" 
      :key="menu.label"
      class="menu-item"
      @click="toggleMenu(menu.label)"
      :class="{ active: activeMenu === menu.label }"
    >
      {{ menu.label }}
      
      <!-- Dropdown Menu -->
      <div 
        v-if="activeMenu === menu.label" 
        class="dropdown-menu"
        @click.stop
      >
        <div
          v-for="item in menu.items"
          :key="item.label"
          class="dropdown-item"
          @click="handleMenuAction(item.action)"
          :class="{ disabled: item.disabled }"
        >
          <span class="item-icon" v-if="item.icon">{{ item.icon }}</span>
          <span class="item-label">{{ item.label }}</span>
          <span class="item-shortcut" v-if="item.shortcut">{{ item.shortcut }}</span>
        </div>
        <div v-if="item.separator" class="dropdown-separator"></div>
      </div>
    </div>
    
    <!-- Click outside overlay -->
    <div 
      v-if="activeMenu" 
      class="menu-overlay" 
      @click="closeMenu"
    ></div>
  </div>
</template>

<script setup>
import { ref } from 'vue'

defineProps({
  menuItems: {
    type: Array,
    required: true
  }
})

const emit = defineEmits(['menu-action'])

const activeMenu = ref(null)

const toggleMenu = (menuLabel) => {
  activeMenu.value = activeMenu.value === menuLabel ? null : menuLabel
}

const closeMenu = () => {
  activeMenu.value = null
}

const handleMenuAction = (action) => {
  emit('menu-action', action)
  closeMenu()
}
</script>

<style scoped>
.menu-bar {
  height: 24px;
  background: #f0f0f0;
  border-bottom: 1px solid #ccc;
  display: flex;
  align-items: center;
  padding: 0 4px;
  flex-shrink: 0;
  position: relative;
  z-index: 100;
}

.menu-item {
  padding: 4px 8px;
  cursor: pointer;
  border-radius: 2px;
  position: relative;
  font-size: 12px;
  user-select: none;
}

.menu-item:hover,
.menu-item.active {
  background: #e0e0e0;
}

.dropdown-menu {
  position: absolute;
  top: 100%;
  left: 0;
  background: white;
  border: 1px solid #999;
  border-radius: 2px;
  box-shadow: 2px 2px 8px rgba(0,0,0,0.15);
  min-width: 160px;
  z-index: 1000;
}

.dropdown-item {
  padding: 6px 12px;
  cursor: pointer;
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 12px;
  border-bottom: 1px solid #f0f0f0;
}

.dropdown-item:last-child {
  border-bottom: none;
}

.dropdown-item:hover:not(.disabled) {
  background: #e0e0e0;
}

.dropdown-item.disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.item-icon {
  font-size: 14px;
  width: 16px;
  text-align: center;
}

.item-label {
  flex: 1;
}

.item-shortcut {
  font-size: 10px;
  color: #666;
  font-family: monospace;
}

.dropdown-separator {
  height: 1px;
  background: #ddd;
  margin: 4px 0;
}

.menu-overlay {
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  z-index: 99;
}
</style>