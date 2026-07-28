<script>
  let { message = '' } = $props();
</script>

{#if message}
  <div class="error-toast">{message}</div>
{/if}
