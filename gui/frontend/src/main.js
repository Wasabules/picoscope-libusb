import './style.css'
import { mount } from 'svelte'
import App from './App.svelte'

// Svelte 5 removed the class component API — `new App({ target })` throws at
// runtime and leaves a blank window. Components are mounted explicitly now.
const app = mount(App, {
  target: document.getElementById('app')
})

export default app
