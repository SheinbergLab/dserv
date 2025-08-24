<template>
  <div class="home">
    <h1>Datapoint Component Lab</h1>
    <nav>
      <router-link to="/terminal">Tcl Terminal</router-link>
      <router-link to="/graphics">Graphics Tester</router-link>
    </nav>
  </div>
</template>

<style scoped>
.home { padding: 40px; text-align: center; }
nav { margin-top: 20px; }
nav a { 
  padding: 10px 20px; 
  background: #007bff; 
  color: white; 
  text-decoration: none; 
  border-radius: 4px; 
  margin: 0 10px;
}
</style>