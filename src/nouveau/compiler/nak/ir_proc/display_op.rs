// Copyright © 2023 Collabora, Ltd.
// SPDX-License-Identifier: MIT

use crate::args::{sources_to_destructure_tokens, DisplayArgs, OpSourceDest};
use crate::modifier::{
    modifiers_to_destructure_tokens, modifiers_to_parser_tokens, Modifier,
};
use proc_macro::TokenStream;
use proc_macro2::{Span, TokenStream as TokenStream2};
use syn::*;

macro_rules! accumulate_error {
    // This macro takes an expression of type `expr` and prints
    // it as a string along with its result.
    // The `expr` designator is used for expressions.
    ($errors:ident, $var:ident) => {
        // `stringify!` will convert the expression *as it is* into a string.
        let $var = match $var {
            Ok(x) => Some(x),
            Err(x) => {
                $errors.push(x);
                None
            }
        };
    };
}

pub fn parser_and<T: quote::ToTokens>(
    mut parsers: impl Iterator<Item = T>,
) -> TokenStream2 {
    let first = parsers.next();
    match first {
        None => quote! { () },
        Some(first) => quote! {
            (#first, #(#parsers,)*).and()
        },
    }
}

pub struct DisplayTokens<T>(pub T);
pub struct ParseTokens<T>(pub T);

pub fn derive_display_op(input: TokenStream) -> TokenStream {
    let DeriveInput {
        ident, data, attrs, ..
    } = parse_macro_input!(input);

    if let Data::Enum(e) = data {
        let mut fmt_dsts_cases = TokenStream2::new();
        let mut fmt_op_cases = TokenStream2::new();
        for v in e.variants {
            let case = v.ident;
            fmt_dsts_cases.extend(quote! {
                #ident::#case(x) => x.fmt_dsts(f),
            });
            fmt_op_cases.extend(quote! {
                #ident::#case(x) => x.fmt_op(f),
            });
        }
        quote! {
            impl DisplayOp for #ident {
                fn fmt_dsts(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                    match self {
                        #fmt_dsts_cases
                    }
                }

                fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                    match self {
                        #fmt_op_cases
                    }
                }
            }
        }
        .into()
    } else if let Data::Struct(s) = data {
        // Missing:
        // - OpFSetP: is_trivial
        // - OpFSwzAdd: custom format
        // - OpDSetP: is_trivial
        // - OpIAdd2X (missing skip if src zero)
        // - OpISetP is_trivial
        // - OpLea if modifier add src(?)
        // - OpLeaX ^
        // - OpShf SRC modifier
        // - OpI2I ^
        // - OpPLop3 what are ops?
        // - OpMov quadlines
        // - OpPrmt custom format split array
        // - OpSuSt: How to print mask?
        // - OpCCtl: op not printed

        // TODO:
        // - modifier after SRC
        // - modifier add "."
        // - op_format for modifier
        // - skip src if 0
        // - AttrAccess

        let args = match attrs.iter().filter(|x|  x.path().is_ident("display_op")).next() {
            Some(attr) => attr.parse_args::<DisplayArgs>(),
            None => {
                ident.to_string()
                    .to_lowercase()
                    .strip_prefix("op")
                    .ok_or_else(|| syn::Error::new(Span::call_site(), "Cannot convert struct name, please use #[display_op(format = )]"))
                    .map(|x| DisplayArgs { format: LitStr::new(x, ident.span()) })
            }
        };

        let modifiers = Modifier::parse_all(&s);
        let dsts = OpSourceDest::parse_all_dsts(&s);
        let srcs = OpSourceDest::parse_all_srcs(&s);

        let mut errors = Vec::new();
        accumulate_error!(errors, args);
        accumulate_error!(errors, modifiers);
        accumulate_error!(errors, srcs);
        accumulate_error!(errors, dsts);

        let error = errors.into_iter().reduce(|mut a, b| {
            a.combine(b);
            a
        });
        if let Some(err) = error {
            return err.into_compile_error().into();
        }

        // No panic, we already handled the errors
        let args = args.unwrap();
        let modifiers = modifiers.unwrap();
        let srcs = srcs.unwrap();
        let dsts = dsts.unwrap();

        let displ_modifiers = modifiers.iter().map(|x| DisplayTokens(x));
        let displ_srcs = srcs.iter().map(|x| DisplayTokens(x));

        let parse_dsts = parser_and(dsts.iter().map(|x| ParseTokens(x)));
        let parse_srcs = parser_and(srcs.iter().map(|x| ParseTokens(x)));
        let parse_mods = modifiers_to_parser_tokens(&modifiers);

        let fmt = args.format;
        let parse_dst_idents = dsts.iter().map(|x| &x.ident);
        let parse_dst_idents2 = parse_dst_idents.clone();
        let parse_mods_idents = modifiers.iter().map(|x| &x.ident);
        let parse_mods_destructure =
            modifiers_to_destructure_tokens(&modifiers);
        let (parse_srcs_destr, parse_srcs_idents) =
            sources_to_destructure_tokens(&srcs);

        let dst_parse = if dsts.is_empty() {
            quote! { () }
        } else {
            quote! {
                crate::parser::terminated(
                    #parse_dsts,
                    crate::parser::whitespace.and(crate::parser::tag("="))
                )
            }
        };
        let q: TokenStream = quote! {
            impl DisplayOp for #ident {
                fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                    write!(f, #fmt)?;
                    #(#displ_modifiers)*
                    #(#displ_srcs)*

                    Ok(())
                }
            }

            impl WithDefaultParser for #ident {
                fn parse<'a>(input: &'a str) -> PResult<'a, Self> {
                    let dst_parser = #dst_parse;
                    let mod_parser = #parse_mods;
                    let src_parser = #parse_srcs;

                    let complete = dst_parser.and(
                        crate::parser::preceded_unique(
                            crate::parser::whitespace.and(crate::parser::tag(#fmt)),
                            mod_parser.and(src_parser)
                        )
                    );
                    complete.map(|((#(#parse_dst_idents,)*), (#parse_mods_destructure, #parse_srcs_destr))| #ident {
                        #(#parse_dst_idents2, )*
                        #(#parse_mods_idents, )*
                        #parse_srcs_idents
                    }).parse(input)
                }
            }
        }
        .into();
        //eprintln!("{}", q.to_string());
        q
    } else {
        panic!("Cannot derive type");
    }
}
