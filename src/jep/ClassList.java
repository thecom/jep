/**
 * Copyright (c) 2015 JEP AUTHORS.
 *
 * This file is licenced under the the zlib/libpng License.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any
 * damages arising from the use of this software.
 * 
 * Permission is granted to anyone to use this software for any
 * purpose, including commercial applications, and to alter it and
 * redistribute it freely, subject to the following restrictions:
 * 
 *     1. The origin of this software must not be misrepresented; you
 *     must not claim that you wrote the original software. If you use
 *     this software in a product, an acknowledgment in the product
 *     documentation would be appreciated but is not required.
 * 
 *     2. Altered source versions must be plainly marked as such, and
 *     must not be misrepresented as being the original software.
 * 
 *     3. This notice may not be removed or altered from any source
 *     distribution.
 */
package jep;

import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.net.URL;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.Set;
import java.util.StringTokenizer;
import java.util.jar.JarEntry;
import java.util.jar.JarFile;

/**
 * A singleton that searches for loaded classes from the JRE and the Java
 * classpath. This is the default ClassEnquirer that is used if no ClassEnquirer
 * is specified when constructing Jep instances. ClassList is also used by the
 * command line <code>jep</code> script.
 * 
 * @author [mrjohnson0 at sourceforge.net] Mike Johnson
 * @version $Id$
 */
public class ClassList implements ClassEnquirer {

    private static ClassList inst;

    // storage for package, members
    private static HashMap<String, ArrayList<String>> packages = new HashMap<String, ArrayList<String>>();

    private ClassList() throws JepException {
        loadClassPath();
        loadPackages();
        loadClassList();
    }

    /**
     * load jar files from class path
     * 
     */
    private void loadClassPath() {
        StringTokenizer tok = new StringTokenizer(
                System.getProperty("java.class.path"),
                System.getProperty("path.separator"));

        while (tok.hasMoreTokens()) {
            String el = tok.nextToken();

            if (!el.toLowerCase().endsWith(".jar"))
                continue; // ignore filesystem classpath

            // make sure it exists
            File file = new File(el);
            if (!file.exists() || !file.canRead())
                continue;

            try {
                JarFile jfile = new JarFile(el, false);
                Enumeration<JarEntry> entries = jfile.entries();
                while (entries.hasMoreElements()) {
                    JarEntry ent = entries.nextElement();

                    if (!ent.getName().toLowerCase().endsWith(".class"))
                        continue;

                    // ent.getName() looks like:
                    // blah.class
                    // jep/ClassList.class
                    int end = ent.getName().lastIndexOf('/');
                    String pname = "default";
                    if (end > 0) {
                        pname = ent.getName().substring(0, end)
                                .replace('/', '.');
                    }

                    String cname = stripClassExt(ent.getName().substring(
                            end + 1));

                    addClass(pname, cname);
                }

                jfile.close();
            } catch (IOException e) {
                // debugging only
                e.printStackTrace();
            }
        }
    }

    /**
     * the jre will tell us about what jar files it has open. use that facility
     * to get a list of packages. then read the files ourselves since java won't
     * share.
     * 
     */
    private void loadPackages() throws JepException {
        ClassLoader cl = this.getClass().getClassLoader();

        Package[] ps = Package.getPackages();
        for (Package p : ps) {
            String pname = p.getName().replace('.', '/');
            URL url = cl.getResource(pname);

            if (url == null || !url.getProtocol().equals("file"))
                continue;

            File dir = null;
            try {
                dir = new File(url.toURI());
            } catch (java.net.URISyntaxException e) {
                throw new JepException(e);
            }

            for (File classfile : dir.listFiles(new ClassFilenameFilter()))
                addClass(p.getName(), stripClassExt(classfile.getName()));
        }
    }

    // don't pass me nulls.
    // strips .class from a file name.
    private String stripClassExt(String name) {
        return name.substring(0, name.length() - 6);
    }

    /**
     * The jre keeps a list of classes in the lib folder. We don't have a better
     * way to figure out what's in the java package, so this is my little hack.
     * 
     */
    private void loadClassList() throws JepException {
        String version = System.getProperty("java.version");

        /*
         * The thread's context ClassLoader is useful if resources have a
         * different ClassLoader than classes (e.g. tomcat), while the Jep.class
         * ClassLoader is useful if running inside an OSGi container as a Bundle
         * (e.g. eclipse).
         */
        ClassLoader[] classloadersToTry = new ClassLoader[] {
                Thread.currentThread().getContextClassLoader(),
                Jep.class.getClassLoader() };
        String rsc = "jep/classlist_";
        if (version.startsWith("1.8")) {
            rsc += "8";
        } else if (version.startsWith("1.7")) {
            rsc += "7";
        } else {
            rsc += "6";
        }
        rsc += ".txt";

        InputStream in = null;
        BufferedReader reader = null;
        ClassLoader cl = null;
        int i = 0;
        try {
            while (in == null && i < classloadersToTry.length) {
                cl = classloadersToTry[i];
                in = cl.getResourceAsStream(rsc);
                i++;
            }

            if (in == null) {
                throw new JepException("ClassList couldn't find resource "
                        + rsc);
            }

            reader = new BufferedReader(new InputStreamReader(in));

            String line = "";
            while ((line = reader.readLine()) != null) {
                // ignore any class with $
                if (line.indexOf('$') > -1)
                    continue;

                // lines in the file look like: java/lang/String
                // split on /
                String[] parts = line.split("\\/");
                StringBuilder pname = new StringBuilder();
                String cname = parts[parts.length - 1];

                for (i = 0; i < parts.length - 1; i++) {
                    pname.append(parts[i]);
                    if (i < parts.length - 2)
                        pname.append(".");
                }

                addClass(pname.toString(), cname);
            }
        } catch (IOException e) {
            throw new JepException(e);
        } finally {
            try {
                if (reader != null)
                    reader.close();
            } catch (IOException ee) {
                // ignore
            }
        }
    }

    // add a class with given package name
    private void addClass(String pname, String cname) {
        ArrayList<String> el = packages.get(pname.toString());
        if (el == null)
            el = new ArrayList<String>();

        // convert to style we need in C code
        if (pname.equals("default")) {
            // don't use this package name, just the class
            ;
        } else
            cname = pname + "." + cname;

        // unlikely, but don't add a class twice.
        if (el.indexOf(cname) > -1)
            return;

        el.add(cname);
        packages.put(pname.toString(), el);
    }

    private String[] _get(String p) {
        ArrayList<String> el = packages.get(p);
        if (el == null) {

            // before we error out, find out if it really is a
            // package. maybe it just doesn't have any classes in it.

            Set<String> keys = packages.keySet();
            for (String key : keys) {
                if (key.startsWith(p))
                    return new String[0];
            }

            return null;
        }

        String[] ret = new String[el.size()];
        el.toArray(ret);
        return ret;
    }

    /**
     * get classnames in package
     * 
     * @param p
     *            a <code>String</code> value
     * @return <code>String[]</code> array of class names
     */
    @Override
    public String[] getClassNames(String p) {
        return _get(p);
    }

    /**
     * classname contained in package
     * 
     * @param p
     *            a <code>String</code> value
     * @return <code>String[]</code> array of class names
     */
    @Override
    public boolean contains(String p) {
        return _get(p) != null;
    }

    @Override
    public boolean supportsPackageImport() {
        return true;
    }

    /**
     * get ClassList instance
     * 
     * @return <code>ClassList</code> instance
     * @exception JepException
     *                if an error occurs
     */
    public static synchronized ClassList getInstance() throws JepException {
        if (ClassList.inst == null)
            ClassList.inst = new ClassList();
        return ClassList.inst;
    }

    /**
     * for testing only
     * 
     * @param argv
     *            command line arguments
     * @throws Throwable
     *             if an error occurs
     */
    public static void main(String argv[]) throws Throwable {
        if (argv.length > 0) {
            for (String arg : argv) {
                for (String c : ClassList.getInstance().getClassNames(arg))
                    System.out.println(c);
            }
        } else {
            for (String c : ClassList.getInstance().getClassNames("java.lang"))
                System.out.println(c);

            // test loadPackages
            for (String c : ClassList.getInstance().getClassNames("jep"))
                System.out.println(c);
        }
    }
}

class ClassFilenameFilter implements java.io.FilenameFilter {
    @Override
    public boolean accept(File dir, String name) {
        return (name != null && name.toLowerCase().endsWith(".class"));
    }
}
