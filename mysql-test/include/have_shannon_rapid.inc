if (`SELECT COUNT(*) = 0 FROM INFORMATION_SCHEMA.ENGINES WHERE engine = 'Rapid' AND support IN ('YES', 'DEFAULT', 'ENABLED')`)
{
  --skip Test requires Shannon Rapid engine.
}
