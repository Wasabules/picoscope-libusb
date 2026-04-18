<script>
  export let message = '';
</script>

{#if message}
  <div class="error-toast">{message}</div>
{/if}
