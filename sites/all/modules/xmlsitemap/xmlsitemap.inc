<?php
// $Id: xmlsitemap.inc,v 1.1.2.17 2009/07/03 04:28:33 davereid Exp $

/**
 * @file
 * Miscellaneous functions for the xmlsitemap module.
 *
 * @ingroup xmlsitemap
 */

/**
 * Given an internal Drupal path, return the alias for the path.
 *
 * This is similar to drupal_get_path_alias(), but designed to fetch all alises
 * at once so that only one database query is executed instead of several or
 * possibly thousands during sitemap generation.
 *
 * @param $path
 *   An internal Drupal path.
 * @param $language
 *   A language code to look use when looking up the paths.
 */
function xmlsitemap_get_path_alias($path, $language) {
  static $aliases;
  static $last_language;

  if (!isset($aliases)) {
    $aliases['all'] = array();
    $query = db_query("SELECT src, dst FROM {url_alias} WHERE language = '' ORDER BY pid");
    while ($alias = db_fetch_object($query)) {
      $aliases['all'][$alias->src] = $alias->dst;
    }
  }
  if ($language && $last_language != $language) {
    unset($aliases[$last_language]);
    $aliases[$language] = array();
    $query = db_query("SELECT src, dst FROM {url_alias} WHERE language = '%s' ORDER BY pid", $language);
    while ($alias = db_fetch_object($query)) {
      $aliases[$language][$alias->src] = $alias->dst;
    }
    $last_language = $language;
  }

  if ($language && isset($aliases[$language][$path])) {
    return $aliases[$language][$path];
  }
  elseif (isset($aliases['all'][$path])) {
    return $aliases['all'][$path];
  }
  else {
    return $path;
  }
}

/**
 * Delete and regenerate the sitemap files.
 */
function xmlsitemap_regenerate() {
  _xmlsitemap_regenerate_before();

  // Generate the sitemap pages.
  $languages = language_list();
  $chunk_count = xmlsitemap_get_chunk_count(TRUE);
  if ($chunk_count > 1) {
    // If we have more than one chunk, we need to increment this value by one
    // since the index page (chunk 0) will also need to be generated.
    $chunk_count++;
  }
  foreach (xmlsitemap_var('languages') as $language) {
    for ($i = 0; $i < $chunk_count; $i++) {
      xmlsitemap_generate($i, $languages[$language]);
    }
  }

  _xmlsitemap_regenerate_after();
}

/**
 * Perform operations before rebuilding the sitemap.
 */
function _xmlsitemap_regenerate_before() {
  // Attempt to increase the available processing time and memory limit.
  @set_time_limit(240);
  _xmlsitemap_set_memory_limit();

  // Set a timer so we can track how long this takes.
  timer_start('xmlsitemap_regenerate');

  // Get the current memory usage so we can track how much memory is used.
  _xmlsitemap_get_memory_usage(TRUE);

  // Clear all cached sitemap files.
  xmlsitemap_clear_cache();

  // Save custom data into a temporary table so it can be re-loaded later.
  //db_query_temporary("SELECT type, id, priority, changefreq, changecount FROM {xmlsitemap}", 'xmlsitemap_temp');
}

function _xmlsitemap_get_memory_usage($start = FALSE) {
  static $memory_start;
  $current = 0;
  if (function_exists('memory_get_peak_usage')) {
    $current = memory_get_peak_usage(TRUE);
  }
  if (function_exists('memory_get_usage')) {
    $current = version_compare(PHP_VERSION, '5.2') ? memory_get_usage(TRUE) : memory_get_usage();
  }
  if (!isset($memory_start) || $start) {
    $memory_start = $current;
  }
  return $current - $memory_start;
}

function _xmlsitemap_get_optimal_memory_limit() {
  static $optimal_limit;
  if (!isset($optimal_limit)) {
    // Set the base memory amount from the provided core constant.
    $optimal_limit = parse_size(DRUPAL_MINIMUM_PHP_MEMORY_LIMIT);

    // Add memory based on the chunk size.
    $optimal_limit += xmlsitemap_get_chunk_size() * 500;

    // Add memory for storing the url aliases.
    $aliases = db_result(db_query("SELECT COUNT(pid) FROM {url_alias} WHERE language = ''"));
    $aliases += db_result(db_query("SELECT COUNT(pid) as pid_count FROM {url_alias} WHERE language <> '' GROUP BY language ORDER BY pid_count DESC"));
    $optimal_limit += $aliases * 250;
  }
  return $optimal_limit;
}

/**
 * Calculate the optimal memory level for sitemap generation.
 */
function _xmlsitemap_set_memory_limit() {
  $memory_limit = @ini_get('memory_limit');
  if ($memory_limit && $memory_limit != -1) {
    $optimal_limit = _xmlsitemap_get_optimal_memory_limit();
    if (parse_size($memory_limit) < $optimal_limit) {
      @ini_set('memory_limit', $optimal_limit);
    }
  }
}

/**
 * Perform operations after rebuilding the sitemap.
 */
function _xmlsitemap_regenerate_after() {
  // Show a watchdog message that the sitemap was regenerated.
  watchdog('xmlsitemap',
    'XML sitemaps regenerated in @timer ms. Memory used: @memory-usage, peak: @memory-peak, optimal: @memory-optimal, limit: @memory-limit.',
    array(
      '@timer' => timer_read('xmlsitemap_regenerate'),
      '@memory-usage' => format_size(_xmlsitemap_get_memory_usage()),
      '@memory-peak' => format_size(memory_get_peak_usage(TRUE)),
      '@memory-limit' => format_size(parse_size(@ini_get('memory_limit'))),
      '@memory-optimal' => format_size(_xmlsitemap_get_optimal_memory_limit()),
    ),
    WATCHDOG_NOTICE,
    l(t('View sitemap'), 'sitemap.xml')
  );

  // Unset the regenerate flag.
  variable_set('xmlsitemap_regenerate_needed', FALSE);

  // If the chunk count has changed, we will need to rebuild the menu.
  variable_set('menu_rebuild_needed', TRUE);

  variable_set('xmlsitemap_generated_last', REQUEST_TIME);
}

/**
 * Fetch the data from {xmlsitemap}, generates the sitemap, then caches it.
 *
 * @param $chunk
 *   An integer representing the integer of the sitemap page chunk.
 * @param $language
 *   A language object, defaults to the default language.
 * @return
 *   TRUE on success; otherwise FALSE
 *
 * @todo Revise/simplify or remove the function.
 */
function xmlsitemap_generate($chunk = 0, $language = NULL) {
  if (!is_numeric($chunk) || $chunk > xmlsitemap_get_chunk_count()) {
    // Don't bother translating this string.
    trigger_error('Improper condition hit in xmlsitemap_generate(). Chunk: ' . $chunk . ', Chunk Count: ' . xmlsitemap_get_chunk_count());
    return FALSE;
  }

  if (!isset($language)) {
    $language = language_default();
  }

  $file = xmlsitemap_get_chunk_file($chunk, $language);

  if (!$handle = fopen($file, 'wb')) {
    watchdog('xmlsitemap', 'Could not open file @file for writing.', array('@file' => $file), WATCHDOG_ERROR);
    return FALSE;
  }

  $status = TRUE;
  if (xmlsitemap_get_chunk_count() > 1 && !$chunk) {
    xmlsitemap_generate_index($handle, $status, $language);
  }
  else {
    xmlsitemap_generate_chunk($handle, $status, $chunk, $language);
  }
  fclose($handle);

  if (!$status) {
    watchdog('xmlsitemap', 'Unknown error occurred while writing to file @file.', array('@file' => $file), WATCHDOG_ERROR);
  }
  elseif (xmlsitemap_var('gz')) {
    $file_gz = xmlsitemap_get_chunk_file($chunk, $language, TRUE);
    file_put_contents($file_gz, gzencode(file_get_contents($file), 9));
  }

  return $status;
}

//function xmlsitemap_fwrite($handle, &$status, $string) {
//  $status &= (bool) fwrite($handle, $string);
//}

/**
 * Write the proper XML sitemap header.
 *
 * @param $handle
 *   A file system pointer resource that is typically created using fopen().
 * @param $status
 * @param $index
 */
function xmlsitemap_generate_chunk_header($handle, &$status, $index = FALSE) {
  $status &= (bool) fwrite($handle, '<?xml version="1.0" encoding="UTF-8"?>' . "\n");
  if (xmlsitemap_var('xsl')) {
    $status &= (bool) fwrite($handle, '<?xml-stylesheet type="text/xsl" href="'. url('sitemap.xsl') .'"?>' . "\n");
  }
  $status &= (bool) fwrite($handle, '<' . ($index ? 'sitemapindex' : 'urlset') . ' xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">' . "\n");
  //$status &= (bool) fwrite($handle, '<' . ($index ? 'sitemapindex' : 'urlset') . ' xmlns="http://www.sitemaps.org/schemas/sitemap/0.9"' . "\n");
  //$status &= (bool) fwrite($handle, '  xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"' . "\n");
  //$status &= (bool) fwrite($handle, '  xsi:schemaLocation="http://www.sitemaps.org/schemas/sitemap/0.9' . "\n");
  //$status &= (bool) fwrite($handle, '  http://www.sitemaps.org/schemas/sitemap/0.9/' . ($index ? 'siteindex.xsd' : 'sitemap.xsd') . '">' . "\n");
  return $status;
}

/**
 * Generate one page (chunk) of the sitemap.
 *
 * @param $handle
 *   A file system pointer resource that is typically created using fopen().
 * @param $status
 * @param $chunk
 *   An integer representing the integer of the sitemap page chunk.
 * @param $language
 *   A language object, defaults to the default language.
 */
function xmlsitemap_generate_chunk($handle, &$status, $chunk, $language = NULL) {
  // @todo When should "AND x.language IN ('%s', '')" be used? Drupal still lists English content when viewing site in French.
  $sql = "SELECT x.loc, x.lastmod, x.changefreq, x.changecount, COALESCE(x.priority_override, x.priority) AS priority FROM {xmlsitemap} x
          WHERE x.access = 1 AND x.status = 1
          ORDER BY x.loc";
  $args = array();
  $url_options = xmlsitemap_get_url_options(array('language' => $language, 'alias' => TRUE));
  $offset = max($chunk - 1, 0) * xmlsitemap_get_chunk_size();
  $limit = xmlsitemap_get_chunk_size();
  $query = db_query_range($sql, $args, $offset, $limit);

  xmlsitemap_generate_chunk_header($handle, $status);
  while ($link = db_fetch_array($query)) {
    $link['alias'] = xmlsitemap_get_path_alias($link['loc'], $language->language);
    $link_output = '<url><loc>' . url($link['alias'], $url_options) . '</loc>';
    if ($link['lastmod']) {
      $link_output .= '<lastmod>' . gmdate(DATE_W3C, $link['lastmod']) . '</lastmod>';
      // If the link has a lastmod value, update the changefreq so that links
      // with a short changefreq but updated two years ago show decay.
      //xmlsitemap_recalculate_changefreq($link);
      $link['changefreq'] = (REQUEST_TIME - $link['lastmod'] + $link['changefreq']) / 2;
    }
    if ($link['changefreq']) {
      $link_output .= '<changefreq>' . xmlsitemap_get_changefreq($link['changefreq']) . '</changefreq>';
    }
    if (isset($link['priority']) && $link['priority'] != 0.5) {
      // Don't output the priority value for links that have 0.5 priority. This
      // is the default 'assumed' value if priority is not included as per the
      // sitemaps.org specification.
      $link_output .= '<priority>' . number_format($link['priority'], 1) . '</priority>';
    }
    $link_output .= '</url>' . PHP_EOL;
    $status &= (bool) fwrite($handle, $link_output);
  }
  $status &= (bool) fwrite($handle, '</urlset>');

  return $status;
}

/**
 * Generate the index sitemap.
 *
 * @param $handle
 *   A file system pointer resource that is typically created using fopen().
 * @param $status
 * @param $language
 *   A language object, defaults to the default language.
 */
function xmlsitemap_generate_index($handle, &$status, $language = NULL) {
  $url_options = xmlsitemap_get_url_options(array('language' => $language, 'alias' => TRUE));
  $chunk_count = xmlsitemap_get_chunk_count(TRUE);

  xmlsitemap_generate_chunk_header($handle, $status, TRUE);
  for ($i = 1; $i <= $chunk_count; $i++) {
    $status &= (bool) fwrite($handle, '<sitemap>');
    $status &= (bool) fwrite($handle, '<loc>' . url('sitemap-' . $i . '.xml', $url_options) . '</loc>');
    // @todo Use the actual lastmod value of the chunk file.
    $status &= (bool) fwrite($handle, '<lastmod>' . gmdate(DATE_W3C, REQUEST_TIME) . '</lastmod>');
    $status &= (bool) fwrite($handle, '</sitemap>' . "\n");
  }
  $status &= (bool) fwrite($handle, '</sitemapindex>');

  return $status;
}

/**
 * Batch callback; clear sitemap links for a specific module.
 */
function xmlsitemap_rebuild_batch_clear($module, &$context) {
  module_invoke($module, 'xmlsitemap_links_clear');
  $context['message'] = t('Now clearing %module links.', array('%module' => $module));
}

/**
 * Batch callback; fetch and add the sitemap links for a specific module.
 */
function xmlsitemap_rebuild_batch_fetch($module, &$context) {
  if (!isset($context['sandbox']['progress'])) {
    $context['sandbox']['batch'] = module_hook($module, 'xmlsitemap_links_batch_info');
    if ($context['sandbox']['batch']) {
      $context['sandbox'] += module_invoke($module, 'xmlsitemap_links_batch_info');
    }
    else {
      $context['sandbox']['links'] = module_invoke($module, 'xmlsitemap_links');
      $context['sandbox']['max'] = count($context['sandbox']['links']);
    }
    $context['sandbox'] += array(
      'progress' => 0,
      'current' => 0,
    );
  }

  if ($context['sandbox']['batch']) {
    $links = module_invoke($module, 'xmlsitemap_links', $context['sandbox']['current'], xmlsitemap_var('batch_limit'));
  }
  else {
    $links = array_splice($context['sandbox']['links'], 0, xmlsitemap_var('batch_limit'));
  }

  foreach ($links as $link) {
    xmlsitemap_save_link($link);
    $context['sandbox']['progress']++;
    $context['sandbox']['current'] = $link['id'];
    $context['message'] = t('Now processing %module link @count.', array('%module' => $module, '@count' => $context['sandbox']['progress']));
  }

  if ($context['sandbox']['progress'] != $context['sandbox']['max']) {
    $context['finished'] = $context['sandbox']['progress'] / $context['sandbox']['max'];
  }
}

/**
 * Batch callback; generate the sitemap chunks for a language.
 */
function xmlsitemap_rebuild_batch_generate($language, &$context) {
  if (!isset($context['sandbox']['progress'])) {
    $context['sandbox']['progress'] = 0;
    $context['sandbox']['max'] = xmlsitemap_get_chunk_count(TRUE);
    // If we have more than one chunk, we need to increment this value by one
    // since the index page (chunk 0) will also need to be generated.
    if ($context['sandbox']['max'] > 1) {
      $context['sandbox']['max']++;
    }
  }

  xmlsitemap_generate($context['sandbox']['progress'], $language);
  $context['sandbox']['progress']++;
  $context['message'] = t('Now generating @language sitemap page @chunk.', array('@language' => $language->name, '@chunk' => $context['sandbox']['progress']));

  if ($context['sandbox']['progress'] != $context['sandbox']['max']) {
    $context['finished'] = $context['sandbox']['progress'] / $context['sandbox']['max'];
  }
}

/**
 * Batch callback; sitemap rebuild finished.
 */
function xmlsitemap_rebuild_batch_finished($success, $results, $operations) {
  if ($success) {
    // Reset the rebuild flag since it was successful.
    variable_set('xmlsitemap_rebuild_needed', FALSE);
    drupal_set_message(t('The sitemap was rebuilt.'));
  }
  else {
    drupal_set_message(t('The sitemap was not successfully rebuilt.'), 'error');
  }
}

/**
 * Fetch a short blurb string about module maintainership and sponsors.
 *
 * This message will be FALSE in 'official' releases.
 */
function _xmlsitemap_get_blurb($check_version = TRUE) {
  static $blurb;

  if (!isset($blurb)) {
    $blurb = FALSE;
    if (!$check_version || preg_match('/dev|alpha|beta/i', _xmlsitemap_get_version())) {
      $sponsors = array(
        l('Symantec', 'http://www.symantec.com/'),
        l('WebWise Solutions', 'http://www.webwiseone.com/'),
        l('Volacci', 'http://www.volacci.com/'),
        l('lanetro', 'http://www.lanetro.com/'),
        l('Coupons Dealuxe', 'http://couponsdealuxe.com/'),
      );
      // Don't extract the following string for translation.
      $blurb = '<p>Thank you for helping test the XML sitemap module rewrite. Please consider helping offset developer free time by <a href="http://davereid.chipin.com/">donating</a> or if your company is interested in sponsoring the rewrite or a specific feature, please <a href="http://davereid.net/contact">contact the developer</a>. Thank you to the following current sponsors: ' . implode(', ', $sponsors) . ', and all the indivduals that have donated. This message will not be seen in the stable versions.</p>';
      //http://drupalmodules.com/module/xml-sitemap
    }
  }

  return $blurb;
}

function _xmlsitemap_get_version() {
  $file = db_fetch_object(db_query("SELECT filename, name, type, status, schema_version FROM {system} WHERE type = 'module' AND name = 'xmlsitemap'"));
  $info = drupal_parse_info_file(drupal_get_path('module', 'xmlsitemap') . '/xmlsitemap.info');
  drupal_alter('system_info', $info, $file);
  return $info['version'];
}

/**
 * Check the status of all hook_requirements() from xmlsitemap modules.
 *
 * @param $return_only
 *   If TRUE, will return the result, otherwise it will show a message.
 * @return
 *   TRUE if there is a warning or error requirement, or FALSE otherwise.
 */
function xmlsitemap_check_status($return_only = FALSE) {
  // Load .install files
  include_once './includes/install.inc';
  drupal_load_updates();
  $warnings = FALSE;

  foreach (module_implements('requirements') as $module) {
    if (strpos($module, 'xmlsitemap') !== FALSE) {
      $requirements = module_invoke($module, 'requirements', 'runtime');
      if (drupal_requirements_severity($requirements) >= REQUIREMENT_WARNING) {
        $warnings = TRUE;
        break;
      }
    }
  }

  if ($warnings && !$return_only && user_access('administer site configuration')) {
    drupal_set_message(t('One or more problems were detected with your sitemap configuration. Please check the <a href="@status-report">status report</a> for more information.', array('@status-report' => url('admin/reports/status'))), 'warning', FALSE);
  }

  return $warnings;
}